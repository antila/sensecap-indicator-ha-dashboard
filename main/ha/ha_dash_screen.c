#include "ha_dash_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ha_dash.h"
#include "ha_dash_present.h"
#include "lv_port.h"
#include "nav.h"
#include "settings_view.h"
#include "view_event.h"

static const char *TAG = "ha-dash-screen";

#define SCREEN_W        480
#define SCREEN_H        480
#define BODY_PAD        16
#define GAP             12
#define HALF_W          ((SCREEN_W - 2 * BODY_PAD - GAP) / 2)
#define WIDE_W          (SCREEN_W - 2 * BODY_PAD)
#define CARD_H          116
#define HEADING_H       30
#define CARD_RADIUS     18
#define SLIDER_HOLD_MS  1500 /* keep the user's slider value until HA catches up */

#define COLOR_BG          0x101418
#define COLOR_CARD        0x1E2328
#define COLOR_CARD_PRESS  0x2B3238
#define COLOR_TEXT        0xF2F2F2
#define COLOR_MUTED       0x9AA3AB
#define COLOR_ICON_IDLE   0x7D868E
#define COLOR_LIGHT       0xFFB74D
#define COLOR_LIGHT_BG    0x3A3020
#define COLOR_SWITCH      0x66BB6A
#define COLOR_SWITCH_BG   0x1C3324
#define COLOR_SENSOR      0x4FC3F7
#define COLOR_SENSOR_BG   0x16303C
#define COLOR_TRACK       0x3A4046
#define COLOR_OK          0x66BB6A
#define COLOR_BUSY        0xFFB74D
#define COLOR_ERROR       0xEF5350

/* Status panel glyphs from ui_font_dash_icons. */
#define GLYPH_COG     "\xEF\x80\x93" /* U+F013 */
#define GLYPH_WIFI    "\xEF\x87\xAB" /* U+F1EB */
#define GLYPH_SYNC    "\xEF\x80\xA1" /* U+F021 */
#define GLYPH_WARNING "\xEF\x81\xB1" /* U+F071 */
#define GLYPH_HOME    "\xEF\x80\x95" /* U+F015 */
#define GLYPH_CHECK   "\xEF\x80\x8C" /* U+F00C */
#define GLYPH_GRID    "\xEF\x80\x89" /* U+F009 */
#define GLYPH_BACK    "\xEF\x81\x93" /* U+F053 */
#define GLYPH_NEXT    "\xEF\x81\x94" /* U+F054 */

LV_FONT_DECLARE(ui_font_dash16);
LV_FONT_DECLARE(ui_font_dash20);
LV_FONT_DECLARE(ui_font_dash32);
LV_FONT_DECLARE(ui_font_dash_icons);

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *state;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *slider;
    uint32_t revision;
    uint32_t slider_hold_until;
    bool fresh;
} tile_widgets_t;

/* Connection status card, shown on the settings page. */
static lv_obj_t *s_status_icon;
static lv_obj_t *s_title;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_text;
static lv_obj_t *s_body;

/* Dashboard picker, opened from the status card. */
static lv_obj_t *s_picker;
static lv_obj_t *s_picker_list;
static uint32_t s_picker_revision = UINT32_MAX;
static char s_picker_current[HA_DASH_PATH_LEN];
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_icon;
static lv_obj_t *s_panel_title;
static lv_obj_t *s_panel_text;

static ha_dash_snapshot_t *s_snap;
static tile_widgets_t s_tiles[HA_DASH_MAX_TILES];
static uint32_t s_built_generation = UINT32_MAX;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

/* A label that stays on one line and ends in "..." when the text is too long.
 * DOTS mode only truncates within a fixed height; without one it wraps. */
static lv_obj_t *make_line_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t width)
{
    lv_obj_t *label = make_label(parent, font, color);
    lv_obj_set_size(label, width, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static lv_obj_t *make_plain_box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void post_action(int index, ha_dash_action_t action, int value)
{
    struct view_data_ha_dash_action payload = {
        .layout_generation = s_built_generation,
        .index = (uint8_t)index,
        .action = (uint8_t)action,
        .value = (int16_t)value,
    };
    if (view_event_post_command(VIEW_EVENT_HA_DASH_ACTION, &payload, sizeof(payload)) !=
        VIEW_EVENT_DELIVERY_ENQUEUED) {
        ESP_LOGW(TAG, "action for tile %d was not delivered", index);
    }
}

static void on_card_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const ha_dash_tile_t *tile = &s_snap->tiles[index];
    if (!tile->tap_enabled || tile->kind == HA_DASH_KIND_SENSOR ||
        tile->kind == HA_DASH_KIND_HEADING || ha_dash_tile_is_unavailable(tile)) {
        return;
    }
    post_action(index, HA_DASH_ACTION_TAP, 0);
}

static void on_slider_event(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    tile_widgets_t *w = &s_tiles[index];
    int value = (int)lv_slider_get_value(w->slider);

    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        char text[24];
        snprintf(text, sizeof(text), value > 0 ? "On \xC2\xB7 %d%%" : "Off", value);
        lv_label_set_text(w->state, text);
    } else {
        w->slider_hold_until = lv_tick_get() + SLIDER_HOLD_MS;
        post_action(index, HA_DASH_ACTION_SET_LEVEL, value);
    }
}

static void style_card(lv_obj_t *card)
{
    lv_obj_set_style_radius(card, CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_PRESS), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(card, 246, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(card, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(card, lv_pct(50), LV_PART_MAIN);
}

static void build_heading(int index)
{
    tile_widgets_t *w = &s_tiles[index];
    w->card = make_plain_box(s_body);
    lv_obj_set_size(w->card, WIDE_W, HEADING_H);
    w->name = make_line_label(w->card, &ui_font_dash16, COLOR_MUTED, WIDE_W - 8);
    lv_obj_align(w->name, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_label_set_text(w->name, s_snap->tiles[index].name);
}

static void build_card(int index)
{
    const ha_dash_tile_t *tile = &s_snap->tiles[index];
    tile_widgets_t *w = &s_tiles[index];
    int width = tile->wide ? WIDE_W : HALF_W;
    bool slider = tile->kind == HA_DASH_KIND_LIGHT;

    w->card = make_plain_box(s_body);
    lv_obj_set_size(w->card, width, CARD_H);
    style_card(w->card);
    bool tappable = tile->tap_enabled && tile->kind != HA_DASH_KIND_SENSOR;
    if (tappable) {
        lv_obj_add_flag(w->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(w->card, on_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    } else {
        lv_obj_remove_flag(w->card, LV_OBJ_FLAG_CLICKABLE);
    }

    w->icon = make_label(w->card, &ui_font_dash_icons, COLOR_ICON_IDLE);
    lv_obj_set_pos(w->icon, 16, 16);

    w->name = make_line_label(w->card, &ui_font_dash16, COLOR_TEXT, width - 32);

    if (tile->kind == HA_DASH_KIND_SENSOR) {
        /* Value and unit share a row in the top-right corner. */
        lv_obj_t *row = make_plain_box(w->card);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
        lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -16, 12);
        w->value = make_label(row, &ui_font_dash32, COLOR_TEXT);
        w->unit = make_label(row, &ui_font_dash16, COLOR_MUTED);
        lv_obj_set_style_pad_bottom(w->unit, 5, LV_PART_MAIN);
        lv_obj_set_style_text_color(w->name, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
        lv_obj_align(w->name, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    } else {
        w->state = make_label(w->card, &ui_font_dash16, COLOR_MUTED);
        lv_obj_align(w->state, LV_ALIGN_TOP_RIGHT, -16, 20);
        lv_obj_align(w->name, LV_ALIGN_BOTTOM_LEFT, 16, slider ? -40 : -16);
    }

    if (slider) {
        w->slider = lv_slider_create(w->card);
        lv_obj_set_size(w->slider, width - 40, 10);
        lv_obj_align(w->slider, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_slider_set_range(w->slider, 0, 100);
        lv_obj_set_style_bg_color(w->slider, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(w->slider, lv_color_hex(COLOR_LIGHT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(w->slider, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(w->slider, 5, LV_PART_KNOB);
        /* A wider touch target than the 10px track. */
        lv_obj_set_ext_click_area(w->slider, 16);
        lv_obj_add_event_cb(w->slider, on_slider_event, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
        lv_obj_add_event_cb(w->slider, on_slider_event, LV_EVENT_RELEASED, (void *)(intptr_t)index);
    }
}

static void rebuild(void)
{
    lv_obj_clean(s_body);
    memset(s_tiles, 0, sizeof(s_tiles));
    for (int i = 0; i < s_snap->tile_count; i++) {
        if (s_snap->tiles[i].kind == HA_DASH_KIND_HEADING) {
            build_heading(i);
        } else {
            build_card(i);
        }
        s_tiles[i].fresh = true;
    }
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    s_built_generation = s_snap->layout_generation;
}

static void tile_colors(const ha_dash_tile_t *tile, uint32_t *accent, uint32_t *bg)
{
    switch (tile->kind) {
        case HA_DASH_KIND_LIGHT:
            *accent = COLOR_LIGHT;
            *bg = COLOR_LIGHT_BG;
            break;
        case HA_DASH_KIND_SENSOR:
            *accent = COLOR_SENSOR;
            *bg = COLOR_SENSOR_BG;
            break;
        default:
            *accent = COLOR_SWITCH;
            *bg = COLOR_SWITCH_BG;
            break;
    }
}

static void update_card(int index)
{
    const ha_dash_tile_t *tile = &s_snap->tiles[index];
    tile_widgets_t *w = &s_tiles[index];
    bool active = ha_dash_tile_is_active(tile);
    bool unavailable = ha_dash_tile_is_unavailable(tile);
    uint32_t accent, active_bg;
    tile_colors(tile, &accent, &active_bg);

    lv_label_set_text(w->icon, ha_dash_icon_glyph(tile));
    lv_obj_set_style_text_color(w->icon, lv_color_hex(active ? accent : COLOR_ICON_IDLE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w->card, lv_color_hex(active ? active_bg : COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_opa(w->card, unavailable && tile->has_state ? LV_OPA_50 : LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_text(w->name, tile->name);

    char text[HA_DASH_STATE_LEN + 16];
    ha_dash_format_state(tile, text, sizeof(text));

    if (w->value) {
        lv_label_set_text(w->value, text);
        lv_label_set_text(w->unit, unavailable ? "" : tile->unit);
    } else {
        lv_label_set_text(w->state, text);
        lv_obj_set_style_text_color(w->state, lv_color_hex(active ? COLOR_TEXT : COLOR_MUTED), LV_PART_MAIN);
    }

    if (w->slider) {
        bool show = tile->dimmable || !tile->has_state;
        if (show) {
            lv_obj_remove_flag(w->slider, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(w->name, LV_ALIGN_BOTTOM_LEFT, 16, -40);
        } else {
            lv_obj_add_flag(w->slider, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(w->name, LV_ALIGN_BOTTOM_LEFT, 16, -16);
        }
        bool held = lv_obj_has_state(w->slider, LV_STATE_PRESSED) ||
                    (int32_t)(w->slider_hold_until - lv_tick_get()) > 0;
        if (!held) {
            int level = active ? (tile->brightness_pct > 0 ? tile->brightness_pct : 100) : 0;
            lv_slider_set_value(w->slider, level, LV_ANIM_ON);
        }
        if (unavailable) {
            lv_obj_add_state(w->slider, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(w->slider, LV_STATE_DISABLED);
        }
    }
}

static void update_status_card(void)
{
    if (s_title == NULL) {
        return;
    }
    const char *title = s_snap->title[0] ? s_snap->title : "Home Assistant";
    for (int i = 0; i < s_snap->board_count; i++) {
        if (strcmp(s_snap->boards[i].url_path, s_snap->dashboard) == 0) {
            title = s_snap->boards[i].title;
            break;
        }
    }
    lv_label_set_text(s_title, title);

    uint32_t dot = COLOR_BUSY;
    const char *state = "Connecting";
    switch (s_snap->status) {
        case HA_DASH_STATUS_READY:
            dot = COLOR_OK;
            state = "Live";
            break;
        case HA_DASH_STATUS_LOADING:
            state = "Loading";
            break;
        case HA_DASH_STATUS_WAITING_NETWORK:
            state = "No Wi-Fi";
            break;
        case HA_DASH_STATUS_UNCONFIGURED:
            dot = COLOR_MUTED;
            state = "Not set up";
            break;
        case HA_DASH_STATUS_AUTH_FAILED:
        case HA_DASH_STATUS_ERROR:
            dot = COLOR_ERROR;
            state = "Error";
            break;
        default:
            break;
    }

    char text[HA_DASH_MESSAGE_LEN + 32];
    if (s_snap->status == HA_DASH_STATUS_READY) {
        int cards = 0;
        for (int i = 0; i < s_snap->tile_count; i++) {
            cards += s_snap->tiles[i].kind != HA_DASH_KIND_HEADING;
        }
        snprintf(text, sizeof(text), "%s \xC2\xB7 %d card%s", state, cards, cards == 1 ? "" : "s");
    } else if (s_snap->message[0]) {
        snprintf(text, sizeof(text), "%s \xC2\xB7 %s", state, s_snap->message);
    } else {
        snprintf(text, sizeof(text), "%s", state);
    }
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(dot), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_icon, lv_color_hex(dot), LV_PART_MAIN);
    lv_label_set_text(s_status_text, text);
}

static void update_panel(void)
{
    const char *icon = GLYPH_SYNC;
    const char *title = "Connecting";
    const char *text = s_snap->message;
    static char error_text[HA_DASH_MESSAGE_LEN + 96];

    switch (s_snap->status) {
        case HA_DASH_STATUS_UNCONFIGURED:
            icon = GLYPH_COG;
            title = "Connect to Home Assistant";
            text = "Swipe left and tap Home Assistant\nto sign in.";
            break;
        case HA_DASH_STATUS_WAITING_NETWORK:
            icon = GLYPH_WIFI;
            title = "Waiting for Wi-Fi";
            text = "Swipe left to open Wi-Fi settings.";
            break;
        case HA_DASH_STATUS_AUTH_FAILED:
            icon = GLYPH_WARNING;
            title = "Signed out";
            snprintf(error_text, sizeof(error_text),
                     "%s\nSwipe left and tap Home Assistant\nto sign in again.", s_snap->message);
            text = error_text;
            break;
        case HA_DASH_STATUS_ERROR:
            icon = GLYPH_WARNING;
            title = "Can't show the dashboard";
            snprintf(error_text, sizeof(error_text),
                     "%s\nSwipe left and tap the dashboard card\nto pick another one.",
                     s_snap->message);
            text = error_text;
            break;
        case HA_DASH_STATUS_READY:
            icon = GLYPH_HOME;
            title = "This dashboard is empty";
            text = "Add cards to it in Home Assistant\nand they will appear here.";
            break;
        default:
            break;
    }

    bool show = s_snap->tile_count == 0;
    if (show) {
        lv_label_set_text(s_panel_icon, icon);
        lv_label_set_text(s_panel_title, title);
        lv_label_set_text(s_panel_text, text);
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        /* Keep the last known tiles visible but dimmed while offline. */
        lv_obj_set_style_opa(s_body,
                             s_snap->status == HA_DASH_STATUS_READY ? LV_OPA_COVER : LV_OPA_60,
                             LV_PART_MAIN);
    }
}

static void picker_close(void)
{
    if (s_picker) {
        lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_picker_back(lv_event_t *e)
{
    (void)e;
    picker_close();
}

static void on_picker_row(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    post_action(0, HA_DASH_ACTION_SELECT_DASHBOARD, index);
    picker_close();
    nav_go_tile(NAV_TILE_HA_DASH);
}

static void picker_message(const char *text)
{
    lv_obj_t *label = make_label(s_picker_list, &ui_font_dash16, COLOR_MUTED);
    lv_obj_set_width(label, WIDE_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 40, LV_PART_MAIN);
    lv_label_set_text(label, text);
}

static void picker_fill(void)
{
    lv_obj_clean(s_picker_list);
    s_picker_revision = s_snap->boards_revision;
    strlcpy(s_picker_current, s_snap->dashboard, sizeof(s_picker_current));

    if (s_snap->board_count == 0) {
        picker_message(s_snap->status == HA_DASH_STATUS_READY ||
                       s_snap->status == HA_DASH_STATUS_LOADING ||
                       s_snap->status == HA_DASH_STATUS_ERROR
                           ? "Loading dashboards..."
                           : "Sign in first: tap Home Assistant\non the settings page.");
        return;
    }

    for (int i = 0; i < s_snap->board_count; i++) {
        const ha_dash_board_t *board = &s_snap->boards[i];
        bool current = strcmp(board->url_path, s_snap->dashboard) == 0;

        lv_obj_t *row = make_plain_box(s_picker_list);
        lv_obj_set_size(row, WIDE_W, 72);
        style_card(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_picker_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        ha_dash_tile_t probe;
        memset(&probe, 0, sizeof(probe));
        strlcpy(probe.icon, board->icon, sizeof(probe.icon));
        lv_obj_t *icon = make_label(row, &ui_font_dash_icons, current ? COLOR_OK : COLOR_ICON_IDLE);
        lv_label_set_text(icon, board->icon[0] ? ha_dash_icon_glyph(&probe) : GLYPH_HOME);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t *title = make_line_label(row, &ui_font_dash20, COLOR_TEXT, WIDE_W - 140);
        lv_label_set_text(title, board->title);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 12);

        char path[HA_DASH_PATH_LEN + 2];
        snprintf(path, sizeof(path), "/%s", board->url_path[0] ? board->url_path : "lovelace");
        lv_obj_t *sub = make_line_label(row, &ui_font_dash16, COLOR_MUTED, WIDE_W - 140);
        lv_label_set_text(sub, path);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 72, -12);

        if (current) {
            lv_obj_t *check = make_label(row, &ui_font_dash_icons, COLOR_OK);
            lv_label_set_text(check, GLYPH_CHECK);
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -20, 0);
        }
    }
}

/* Full-screen pop-up on the top layer with a Back button and a title. */
static lv_obj_t *make_modal(const char *title_text, lv_event_cb_t on_back)
{
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(modal, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE); /* swallow taps behind it */
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *back = make_plain_box(modal);
    lv_obj_set_size(back, 110, 52);
    lv_obj_set_pos(back, 8, 14);
    lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_CARD_PRESS), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = make_label(back, &ui_font_dash16, COLOR_TEXT);
    lv_label_set_text(back_icon, "Back");
    lv_obj_align(back_icon, LV_ALIGN_LEFT_MID, 38, 0);
    lv_obj_t *chevron = make_label(back, &ui_font_dash_icons, COLOR_TEXT);
    lv_obj_set_style_transform_scale(chevron, 160, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(chevron, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(chevron, lv_pct(50), LV_PART_MAIN);
    lv_label_set_text(chevron, GLYPH_BACK);
    lv_obj_align(chevron, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *title = make_label(modal, &ui_font_dash20, COLOR_TEXT);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    return modal;
}

/* Text button for a modal header's right side. */
static lv_obj_t *make_header_button(lv_obj_t *modal, const char *text, uint32_t color,
                                    lv_event_cb_t cb)
{
    lv_obj_t *button = make_plain_box(modal);
    lv_obj_set_size(button, 110, 52);
    lv_obj_set_pos(button, SCREEN_W - 110 - 8, 14);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = make_label(button, &ui_font_dash16, COLOR_TEXT);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void picker_create(void)
{
    s_picker = make_modal("Choose dashboard", on_picker_back);

    s_picker_list = make_plain_box(s_picker);
    lv_obj_add_flag(s_picker_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_picker_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_size(s_picker_list, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(s_picker_list, 0, 80);
    lv_obj_set_style_pad_hor(s_picker_list, BODY_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_picker_list, BODY_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_picker_list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_picker_list, LV_FLEX_FLOW_COLUMN);
}

static void on_status_card_clicked(lv_event_t *e)
{
    (void)e;
    if (s_picker == NULL) {
        picker_create();
    }
    picker_fill();
    lv_obj_scroll_to_y(s_picker_list, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker);
    post_action(0, HA_DASH_ACTION_LIST_DASHBOARDS, 0); /* pick up new dashboards */
}

static void update_picker(void)
{
    if (s_picker == NULL || lv_obj_has_flag(s_picker, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (s_picker_revision != s_snap->boards_revision ||
        strcmp(s_picker_current, s_snap->dashboard) != 0) {
        picker_fill();
    }
}

/* ── Sign-in form ───────────────────────────────────────────────────────── */

#define LOGIN_FIELD_H   46
#define LOGIN_FIELD_Y   80
#define LOGIN_FIELD_GAP 8
#define LOGIN_KB_H      200

static lv_obj_t *s_login;
static lv_obj_t *s_login_address;
static lv_obj_t *s_login_user;
static lv_obj_t *s_login_pass;
static lv_obj_t *s_login_status;
static lv_obj_t *s_login_button;
static lv_obj_t *s_login_kb;
static bool s_login_waiting;
static uint32_t s_login_wait_revision;

static void login_set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_login_status, text);
    lv_obj_set_style_text_color(s_login_status, lv_color_hex(color), LV_PART_MAIN);
}

static void login_close(void)
{
    if (s_login) {
        lv_obj_add_flag(s_login_kb, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_login_pass, "");
        lv_obj_add_flag(s_login, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_login_back(lv_event_t *e)
{
    (void)e;
    login_close();
}

static void login_submit(void)
{
    const char *address = lv_textarea_get_text(s_login_address);
    const char *user = lv_textarea_get_text(s_login_user);
    const char *pass = lv_textarea_get_text(s_login_pass);
    if (address[0] == '\0' || user[0] == '\0' || pass[0] == '\0') {
        login_set_status("Fill in address, username and password", COLOR_ERROR);
        return;
    }
    lv_obj_add_flag(s_login_kb, LV_OBJ_FLAG_HIDDEN);
    s_login_waiting = true;
    s_login_wait_revision = s_snap->login_revision;
    lv_obj_add_state(s_login_button, LV_STATE_DISABLED);
    login_set_status("Signing in...", COLOR_MUTED);
    ha_dash_login(address, user, pass);
    lv_textarea_set_text(s_login_pass, "");
}

static void on_login_button(lv_event_t *e)
{
    (void)e;
    if (!s_login_waiting) {
        login_submit();
    }
}

static void on_login_field(lv_event_t *e)
{
    lv_obj_t *field = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(s_login_kb, field);
    lv_obj_remove_flag(s_login_kb, LV_OBJ_FLAG_HIDDEN);
}

static void on_login_keyboard(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_login_kb, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Enter moves to the next field, and signs in from the password. */
    lv_obj_t *field = lv_keyboard_get_textarea(s_login_kb);
    if (field == s_login_address) {
        lv_obj_send_event(s_login_user, LV_EVENT_CLICKED, NULL);
    } else if (field == s_login_user) {
        lv_obj_send_event(s_login_pass, LV_EVENT_CLICKED, NULL);
    } else if (!s_login_waiting) {
        login_submit();
    }
}

static lv_obj_t *login_field(int32_t y, const char *placeholder, bool password)
{
    lv_obj_t *field = lv_textarea_create(s_login);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_placeholder_text(field, placeholder);
    lv_textarea_set_password_mode(field, password);
    lv_textarea_set_max_length(field, password ? 63 : 120);
    lv_obj_set_size(field, WIDE_W, LOGIN_FIELD_H);
    lv_obj_set_pos(field, BODY_PAD, y);
    lv_obj_set_style_text_font(field, &ui_font_dash20, LV_PART_MAIN);
    lv_obj_set_style_text_color(field, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_color(field, lv_color_hex(COLOR_ICON_IDLE), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(field, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(field, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(field, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(field, lv_color_hex(COLOR_SENSOR), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(field, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(field, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(field, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(field, on_login_field, LV_EVENT_CLICKED, NULL);
    return field;
}

static void login_create(void)
{
    s_login = make_modal("Sign in", on_login_back);
    s_login_button = make_header_button(s_login, "Sign in", 0x2E7D9A, on_login_button);

    s_login_address = login_field(LOGIN_FIELD_Y, "Address, e.g. 192.168.1.10:8123", false);
    s_login_user = login_field(LOGIN_FIELD_Y + LOGIN_FIELD_H + LOGIN_FIELD_GAP, "Username", false);
    s_login_pass = login_field(LOGIN_FIELD_Y + 2 * (LOGIN_FIELD_H + LOGIN_FIELD_GAP), "Password", true);

    /* Sits just above the keyboard so it stays visible while typing. */
    s_login_status = make_line_label(s_login, &ui_font_dash16, COLOR_MUTED, WIDE_W);
    lv_obj_set_pos(s_login_status, BODY_PAD + 4, LOGIN_FIELD_Y + 3 * (LOGIN_FIELD_H + LOGIN_FIELD_GAP) + 2);

    s_login_kb = lv_keyboard_create(s_login);
    lv_obj_set_size(s_login_kb, SCREEN_W, LOGIN_KB_H);
    lv_obj_align(s_login_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_login_kb, on_login_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_login_kb, on_login_keyboard, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_login_kb, LV_OBJ_FLAG_HIDDEN);
}

static void login_open(void)
{
    if (s_login == NULL) {
        login_create();
    }
    lv_textarea_set_text(s_login_address, s_snap->server);
    lv_textarea_set_text(s_login_user, s_snap->username);
    lv_textarea_set_text(s_login_pass, "");
    s_login_waiting = false;
    lv_obj_remove_state(s_login_button, LV_STATE_DISABLED);
    if (s_snap->username[0]) {
        char text[HA_DASH_NAME_LEN + 24];
        snprintf(text, sizeof(text), "Signed in as %s", s_snap->username);
        login_set_status(text, COLOR_MUTED);
    } else {
        login_set_status("Your password is used once and not stored.", COLOR_MUTED);
    }
    lv_obj_remove_flag(s_login, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_login);
    /* Start typing in the first empty field. */
    lv_obj_t *first = s_snap->server[0] == '\0' ? s_login_address
                      : s_snap->username[0] == '\0' ? s_login_user
                                                     : s_login_pass;
    lv_obj_send_event(first, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(first, LV_STATE_FOCUSED);
}

static void update_login(void)
{
    if (s_login == NULL || !s_login_waiting || s_snap->login_revision == s_login_wait_revision) {
        return;
    }
    s_login_waiting = false;
    lv_obj_remove_state(s_login_button, LV_STATE_DISABLED);
    if (s_snap->login_ok) {
        login_close();
        picker_close();
        nav_go_tile(NAV_TILE_HA_DASH);
    } else {
        login_set_status(s_snap->login_message, COLOR_ERROR);
    }
}

static void render(void)
{
    if (s_snap->layout_generation != s_built_generation) {
        rebuild();
    }
    update_status_card();
    update_picker();
    update_login();
    update_panel();
    for (int i = 0; i < s_snap->tile_count; i++) {
        tile_widgets_t *w = &s_tiles[i];
        if (s_snap->tiles[i].kind == HA_DASH_KIND_HEADING) {
            continue;
        }
        if (w->fresh || w->revision != s_snap->tiles[i].revision) {
            update_card(i);
            w->revision = s_snap->tiles[i].revision;
            w->fresh = false;
        }
    }
}

/* Settings page "Home Assistant" button. */
static void on_screen_start(void *context, enum view_event_id id, const void *payload)
{
    if (*(const uint8_t *)payload != SCREEN_HA_LOGIN) {
        return;
    }
    lv_port_sem_take();
    if (s_snap) {
        login_open();
    }
    lv_port_sem_give();
}

static void on_dash_changed(void *context, enum view_event_id id, const void *payload)
{
    /* Copy under the LVGL lock: tap callbacks read s_snap on the LVGL task. */
    lv_port_sem_take();
    if (ha_dash_snapshot(s_snap)) {
        render();
    }
    lv_port_sem_give();
}

static void create_status_card(lv_obj_t *slot)
{
    if (slot == NULL) {
        return; /* no settings page, e.g. in a reduced build */
    }
    lv_obj_set_style_radius(slot, CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot, lv_color_hex(COLOR_CARD_PRESS), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, on_status_card_clicked, LV_EVENT_CLICKED, NULL);
    int32_t width = lv_obj_get_style_width(slot, LV_PART_MAIN);

    lv_obj_t *chevron = make_label(slot, &ui_font_dash_icons, COLOR_MUTED);
    lv_obj_set_style_transform_scale(chevron, 160, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(chevron, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(chevron, lv_pct(50), LV_PART_MAIN);
    lv_label_set_text(chevron, GLYPH_NEXT);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);

    s_status_icon = make_label(slot, &ui_font_dash_icons, COLOR_MUTED);
    lv_label_set_text(s_status_icon, GLYPH_GRID);
    lv_obj_align(s_status_icon, LV_ALIGN_LEFT_MID, 22, 0);

    s_title = make_line_label(slot, &ui_font_dash20, COLOR_TEXT, width - 130);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 76, 24);

    lv_obj_t *status = make_plain_box(slot);
    lv_obj_set_size(status, width - 130, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 8, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 76, -22);

    s_status_dot = make_plain_box(status);
    lv_obj_set_size(s_status_dot, 10, 10);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    s_status_text = make_line_label(status, &ui_font_dash16, COLOR_MUTED, width - 148);
}

static void create_body(lv_obj_t *tile)
{
    s_body = make_plain_box(tile);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_size(s_body, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_body, 0, 0);
    lv_obj_set_style_pad_hor(s_body, BODY_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_body, BODY_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_body, BODY_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_body, GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_body, GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW_WRAP);
}

static void create_panel(lv_obj_t *tile)
{
    s_panel = make_plain_box(tile);
    lv_obj_set_size(s_panel, WIDE_W, LV_SIZE_CONTENT);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 12, LV_PART_MAIN);

    s_panel_icon = make_label(s_panel, &ui_font_dash_icons, COLOR_MUTED);
    lv_obj_set_style_pad_bottom(s_panel_icon, 6, LV_PART_MAIN);
    s_panel_title = make_label(s_panel, &ui_font_dash20, COLOR_TEXT);
    s_panel_text = make_label(s_panel, &ui_font_dash16, COLOR_MUTED);
    lv_obj_set_style_text_align(s_panel_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_panel_text, 4, LV_PART_MAIN);
}

void ha_dash_screen_create(void)
{
    /* ~10 KB: keep it out of internal RAM, which the drivers need. */
    s_snap = heap_caps_calloc(1, sizeof(*s_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_snap == NULL) {
        s_snap = calloc(1, sizeof(*s_snap));
    }
    if (s_snap == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    s_snap->status = HA_DASH_STATUS_CONNECTING;

    lv_port_sem_take();
    lv_obj_t *tile = nav_get_tile(NAV_TILE_HA_DASH);
    if (tile == NULL) {
        lv_port_sem_give();
        ESP_LOGE(TAG, "dashboard tile is not initialized");
        return;
    }
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    create_body(tile);
    create_panel(tile);
    create_status_card(settings_view_ha_slot());
    render();
    lv_port_sem_give();

    if (!view_event_subscribe(VIEW_EVENT_HA_DASH_CHANGED, on_dash_changed, NULL) ||
        !view_event_subscribe(VIEW_EVENT_SCREEN_START, on_screen_start, NULL)) {
        ESP_LOGE(TAG, "failed to subscribe to dashboard changes");
    }
}
