#include "settings_view.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lv_port.h"
#include "nav.h"
#include "sdkconfig.h"
#include "view_data.h"
#include "view_event.h"

LV_FONT_DECLARE(ui_font_dash16);
LV_FONT_DECLARE(ui_font_dash20);
LV_FONT_DECLARE(ui_font_dash_icons);

static const char *TAG = "settings-view";

/* The settings page is the tile right of the dashboard: swipe left to open,
 * swipe right to go back. */
#define PAGE_W          480
#define PAD             16
#define GAP             14
#define TITLE_Y         22
#define HA_SLOT_Y       62
#define HA_SLOT_H       104
#define ACTIONS_Y       182
#define ACTION_W        ((PAGE_W - 2 * PAD - 2 * GAP) / 3)
#define ACTION_H        118
#define INFO_Y          316
#define INFO_H          96
#define IP_REFRESH_MS   3000

#define COLOR_CARD       0x1E2328
#define COLOR_CARD_PRESS 0x2B3238
#define COLOR_TEXT       0xF2F2F2
#define COLOR_MUTED      0x9AA3AB

/* Glyphs from ui_font_dash_icons. */
#define GLYPH_WIFI  "\xEF\x87\xAB" /* U+F1EB */
#define GLYPH_SUN   "\xEF\x86\x85" /* U+F185 */
#define GLYPH_GLOBE "\xEF\x82\xAC" /* U+F0AC */
#define GLYPH_HOME  "\xEF\x80\x95" /* U+F015 */

/* The MQTT broker button is hidden for now; the dashboard doesn't use MQTT. */
#define SETTINGS_SHOW_MQTT 0

static lv_obj_t *s_settings_page = NULL;
static lv_obj_t *s_ha_slot = NULL;
static lv_obj_t *s_settings_ip_label = NULL;

static void settings_refresh_ip_label(void)
{
	if(!s_settings_ip_label)
	{
		return;
	}

	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip_info = {0};

	if(netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
	   ip_info.ip.addr != 0)
	{
		char buf[40];
		snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
		lv_label_set_text(s_settings_ip_label, buf);
	}
	else
	{
		lv_label_set_text(s_settings_ip_label, "Not connected");
	}
}

static void settings_on_ip_timer(lv_timer_t *timer)
{
	(void)timer;
	settings_refresh_ip_label();
}

static bool settings_post_screen(enum start_screen screen)
{
	uint8_t screen_id = (uint8_t)screen;
	view_event_delivery_t delivery = view_event_post_command(
		VIEW_EVENT_SCREEN_START, &screen_id, sizeof(screen_id));
	if(delivery != VIEW_EVENT_DELIVERY_ENQUEUED)
	{
		ESP_LOGE(TAG, "screen command rejected: delivery=%d", (int)delivery);
		return false;
	}
	return true;
}

static void settings_open_wifi(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		(void)settings_post_screen(SCREEN_WIFI_CONFIG);
	}
}

static void settings_open_display(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		(void)settings_post_screen(SCREEN_DISPLAY_MODAL);
	}
}

static void settings_open_home_assistant(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		(void)settings_post_screen(SCREEN_HA_LOGIN);
	}
}

#if SETTINGS_SHOW_MQTT
static void settings_open_broker(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		(void)settings_post_screen(SCREEN_BROKER_MODAL);
	}
}
#endif

void settings_view_show(void)
{
	if(!s_settings_page)
	{
		return;
	}
	settings_refresh_ip_label();
	nav_go_tile(NAV_TILE_SETTINGS);
}

lv_obj_t *settings_view_ha_slot(void)
{
	return s_ha_slot;
}

static lv_obj_t *settings_label(lv_obj_t *parent, const lv_font_t *font,
								uint32_t color, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
	lv_label_set_text(label, text);
	return label;
}

static lv_obj_t *settings_box(lv_obj_t *parent, int32_t x, int32_t y,
							  int32_t w, int32_t h)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_pos(box, x, y);
	lv_obj_set_size(box, w, h);
	return box;
}

static void settings_style_card(lv_obj_t *card)
{
	lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
	lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_PRESS),
							  LV_PART_MAIN | LV_STATE_PRESSED);
}

static void settings_create_action(lv_obj_t *parent, int32_t x, uint32_t accent,
								   const char *glyph, const char *label,
								   lv_event_cb_t cb)
{
	lv_obj_t *card = settings_box(parent, x, ACTIONS_Y, ACTION_W, ACTION_H);
	settings_style_card(card);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = settings_label(card, &ui_font_dash_icons, accent, glyph);
	lv_obj_align(icon, LV_ALIGN_CENTER, 0, -16);

	lv_obj_t *title = settings_label(card, &ui_font_dash16, COLOR_TEXT, label);
	lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static lv_obj_t *settings_info_row(lv_obj_t *parent, int32_t y, const char *name,
								   const char *value)
{
	lv_obj_t *key = settings_label(parent, &ui_font_dash16, COLOR_MUTED, name);
	lv_obj_align(key, LV_ALIGN_TOP_LEFT, 20, y);
	lv_obj_t *val = settings_label(parent, &ui_font_dash16, COLOR_TEXT, value);
	lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -20, y);
	return val;
}

static void settings_create_page(lv_obj_t *tile)
{
	s_settings_page = tile;

	lv_obj_t *title = settings_label(tile, &ui_font_dash20, COLOR_TEXT, "Settings");
	lv_obj_set_pos(title, PAD + 4, TITLE_Y);

	s_ha_slot = settings_box(tile, PAD, HA_SLOT_Y, PAGE_W - 2 * PAD, HA_SLOT_H);

	settings_create_action(tile, PAD, 0x4EACE4, GLYPH_WIFI, "Wi-Fi",
						   settings_open_wifi);
	settings_create_action(tile, PAD + ACTION_W + GAP, 0xEEBF41, GLYPH_SUN,
						   "Display", settings_open_display);
	settings_create_action(tile, PAD + 2 * (ACTION_W + GAP), 0x41BDF5,
						   GLYPH_HOME, "Home Assistant", settings_open_home_assistant);
#if SETTINGS_SHOW_MQTT
	settings_create_action(tile, PAD + 2 * (ACTION_W + GAP), 0xE66D39,
						   GLYPH_GLOBE, "MQTT", settings_open_broker);
#endif

	lv_obj_t *info = settings_box(tile, PAD, INFO_Y, PAGE_W - 2 * PAD, INFO_H);
	settings_style_card(info);
	s_settings_ip_label = settings_info_row(info, 20, "IP address", "Not connected");
	settings_info_row(info, 56, "Firmware", esp_app_get_description()->version);
	settings_refresh_ip_label();
	lv_timer_create(settings_on_ip_timer, IP_REFRESH_MS, NULL);

	lv_obj_t *hint = settings_label(tile, &ui_font_dash16, COLOR_MUTED,
									"Swipe right for the dashboard");
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

int settings_view_init(void)
{
	lv_port_sem_take();
	lv_obj_t *tile = nav_get_tile(NAV_TILE_SETTINGS);
	if(!tile)
	{
		lv_port_sem_give();
		ESP_LOGE(TAG, "Settings tile is not initialized");
		return -1;
	}
	settings_create_page(tile);
	lv_port_sem_give();
	return 0;
}
