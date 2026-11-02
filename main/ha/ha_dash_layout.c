#include "ha_dash_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nested stacks deeper than this are ignored rather than recursed into. */
#define LAYOUT_MAX_DEPTH 6

/* Copy src into dst, truncating on a UTF-8 character boundary. */
static void copy_text(char *dst, size_t len, const char *src)
{
    if (len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= len) {
        n = len - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--; /* don't split a multi-byte character */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

bool ha_dash_websocket_url(const char *input, char *out, size_t len)
{
    static const struct {
        const char *prefix;
        const char *scheme;
    } SCHEMES[] = {
        {"https://", "wss://"}, {"http://", "ws://"}, {"wss://", "wss://"}, {"ws://", "ws://"},
    };

    if (input == NULL || input[0] == '\0') {
        return false;
    }
    const char *scheme = NULL;
    const char *rest = input;
    for (size_t i = 0; i < sizeof(SCHEMES) / sizeof(SCHEMES[0]); i++) {
        size_t n = strlen(SCHEMES[i].prefix);
        if (strncmp(input, SCHEMES[i].prefix, n) == 0) {
            scheme = SCHEMES[i].scheme;
            rest = input + n;
            break;
        }
    }

    const char *slash = strchr(rest, '/');
    size_t host_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (host_len == 0) {
        return false;
    }
    bool has_port = memchr(rest, ':', host_len) != NULL;
    const char *path = (slash && slash[1] != '\0') ? slash : "/api/websocket";

    int n = snprintf(out, len, "%s%.*s%s%s", scheme ? scheme : "ws://", (int)host_len, rest,
                     (scheme == NULL && !has_port) ? ":8123" : "", path);
    return n > 0 && (size_t)n < len;
}

static bool domain_in(const char *domain, const char *const *list)
{
    for (; *list; list++) {
        if (strcmp(domain, *list) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const TOGGLE_DOMAINS[] = {
    "switch", "input_boolean", "fan", "automation", "siren", "humidifier",
    "cover", "lock", "valve", "media_player", NULL,
};

static const char *const BUTTON_DOMAINS[] = {
    "script", "scene", "button", "input_button", NULL,
};

static ha_dash_kind_t kind_for_entity(const char *entity_id)
{
    char domain[32];
    if (!ha_dash_entity_domain(entity_id, domain, sizeof(domain))) {
        return HA_DASH_KIND_SENSOR;
    }
    if (strcmp(domain, "light") == 0) {
        return HA_DASH_KIND_LIGHT;
    }
    if (domain_in(domain, TOGGLE_DOMAINS)) {
        return HA_DASH_KIND_TOGGLE;
    }
    if (domain_in(domain, BUTTON_DOMAINS)) {
        return HA_DASH_KIND_BUTTON;
    }
    return HA_DASH_KIND_SENSOR;
}

static bool card_is_wide(const cJSON *card)
{
    const cJSON *grid = cJSON_GetObjectItemCaseSensitive(card, "grid_options");
    const cJSON *columns = cJSON_GetObjectItemCaseSensitive(grid, "columns");
    if (cJSON_IsString(columns)) {
        return strcmp(columns->valuestring, "full") == 0;
    }
    /* Sections use a 12-column grid; anything over three quarters reads as a
     * full-width card on a two-column screen. */
    return cJSON_IsNumber(columns) && columns->valuedouble >= 9;
}

static bool tap_enabled(const cJSON *card)
{
    const cJSON *tap = cJSON_GetObjectItemCaseSensitive(card, "tap_action");
    const char *action = json_string(tap, "action");
    return action == NULL || strcmp(action, "none") != 0;
}

static ha_dash_tile_t *next_tile(ha_dash_snapshot_t *out)
{
    if (out->tile_count >= HA_DASH_MAX_TILES) {
        return NULL;
    }
    ha_dash_tile_t *tile = &out->tiles[out->tile_count++];
    memset(tile, 0, sizeof(*tile));
    tile->brightness_pct = -1;
    return tile;
}

static void add_heading(ha_dash_snapshot_t *out, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    ha_dash_tile_t *tile = next_tile(out);
    if (tile) {
        tile->kind = HA_DASH_KIND_HEADING;
        tile->wide = true;
        copy_text(tile->name, sizeof(tile->name), text);
    }
}

/* `source` is the card or entities-row object carrying name/icon overrides. */
static void add_entity(ha_dash_snapshot_t *out, const char *entity_id,
                       const cJSON *source, bool wide)
{
    if (entity_id == NULL || strchr(entity_id, '.') == NULL) {
        return;
    }
    ha_dash_tile_t *tile = next_tile(out);
    if (tile == NULL) {
        return;
    }
    tile->kind = kind_for_entity(entity_id);
    tile->wide = wide;
    tile->tap_enabled = source ? tap_enabled(source) : true;
    copy_text(tile->entity_id, sizeof(tile->entity_id), entity_id);

    const char *name = source ? json_string(source, "name") : NULL;
    tile->name_from_card = name && name[0];
    copy_text(tile->name, sizeof(tile->name), tile->name_from_card ? name : entity_id);

    const char *icon = source ? json_string(source, "icon") : NULL;
    tile->icon_from_card = icon && icon[0];
    copy_text(tile->icon, sizeof(tile->icon), tile->icon_from_card ? icon : "");
}

static void add_entities_list(ha_dash_snapshot_t *out, const cJSON *entities)
{
    const cJSON *row;
    cJSON_ArrayForEach(row, entities) {
        if (cJSON_IsString(row)) {
            add_entity(out, row->valuestring, NULL, false);
        } else if (cJSON_IsObject(row)) {
            const char *entity = json_string(row, "entity");
            const char *type = json_string(row, "type");
            if (entity) {
                add_entity(out, entity, row, false);
            } else if (type && strcmp(type, "section") == 0) {
                add_heading(out, json_string(row, "label"));
            }
        }
    }
}

static void add_card(ha_dash_snapshot_t *out, const cJSON *card, int depth)
{
    if (!cJSON_IsObject(card) || depth > LAYOUT_MAX_DEPTH) {
        return;
    }
    const char *type = json_string(card, "type");
    if (type == NULL) {
        return;
    }

    if (strcmp(type, "heading") == 0) {
        add_heading(out, json_string(card, "heading"));
        return;
    }
    if (strcmp(type, "conditional") == 0) {
        add_card(out, cJSON_GetObjectItemCaseSensitive(card, "card"), depth + 1);
        return;
    }

    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(card, "cards");
    if (cJSON_IsArray(cards)) { /* vertical-stack, horizontal-stack, grid */
        add_heading(out, json_string(card, "title"));
        const cJSON *child;
        cJSON_ArrayForEach(child, cards) {
            add_card(out, child, depth + 1);
        }
        return;
    }

    const char *entity = json_string(card, "entity");
    if (entity) {
        add_entity(out, entity, card, card_is_wide(card));
        return;
    }

    const cJSON *entities = cJSON_GetObjectItemCaseSensitive(card, "entities");
    if (cJSON_IsArray(entities)) { /* entities, glance, history-graph, ... */
        add_heading(out, json_string(card, "title"));
        add_entities_list(out, entities);
    }
}

int ha_dash_layout_parse(const cJSON *config, ha_dash_snapshot_t *out)
{
    out->tile_count = 0;
    out->title[0] = '\0';

    const cJSON *views = cJSON_GetObjectItemCaseSensitive(config, "views");
    const cJSON *view = cJSON_GetArrayItem(views, 0);
    if (!cJSON_IsObject(view)) {
        return -1;
    }
    copy_text(out->title, sizeof(out->title), json_string(view, "title"));

    const cJSON *sections = cJSON_GetObjectItemCaseSensitive(view, "sections");
    const cJSON *section;
    cJSON_ArrayForEach(section, sections) {
        add_heading(out, json_string(section, "title"));
        const cJSON *card;
        cJSON_ArrayForEach(card, cJSON_GetObjectItemCaseSensitive(section, "cards")) {
            add_card(out, card, 0);
        }
    }

    const cJSON *card;
    cJSON_ArrayForEach(card, cJSON_GetObjectItemCaseSensitive(view, "cards")) {
        add_card(out, card, 0);
    }
    return out->tile_count;
}

int ha_dash_parse_dashboards(const cJSON *result, ha_dash_board_t *out, int max)
{
    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, result) {
        const char *path = json_string(item, "url_path");
        if (path == NULL || count >= max - 1) { /* keep room for the default */
            continue;
        }
        ha_dash_board_t *board = &out[count++];
        copy_text(board->url_path, sizeof(board->url_path), path);
        const char *title = json_string(item, "title");
        copy_text(board->title, sizeof(board->title), title && title[0] ? title : path);
        copy_text(board->icon, sizeof(board->icon), json_string(item, "icon"));
    }
    if (count < max) {
        ha_dash_board_t *board = &out[count++];
        copy_text(board->url_path, sizeof(board->url_path), "");
        copy_text(board->title, sizeof(board->title), "Default dashboard");
        copy_text(board->icon, sizeof(board->icon), "mdi:home");
    }
    return count;
}

int ha_dash_layout_entity_ids_json(const ha_dash_snapshot_t *snap, char *buf, size_t len)
{
    size_t used = 0;
    int n = snprintf(buf, len, "[");
    if (n < 0 || (size_t)n >= len) {
        return -1;
    }
    used = (size_t)n;
    bool first = true;
    for (int i = 0; i < snap->tile_count; i++) {
        const char *id = snap->tiles[i].entity_id;
        if (id[0] == '\0') {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(snap->tiles[j].entity_id, id) == 0;
        }
        if (seen) {
            continue;
        }
        n = snprintf(buf + used, len - used, "%s\"%s\"", first ? "" : ",", id);
        if (n < 0 || (size_t)n >= len - used) {
            return -1;
        }
        used += (size_t)n;
        first = false;
    }
    n = snprintf(buf + used, len - used, "]");
    if (n < 0 || (size_t)n >= len - used) {
        return -1;
    }
    return (int)(used + (size_t)n);
}

static void apply_attributes(ha_dash_tile_t *tile, const cJSON *attrs)
{
    if (!cJSON_IsObject(attrs)) {
        return;
    }
    const char *text;
    if (!tile->name_from_card && (text = json_string(attrs, "friendly_name"))) {
        copy_text(tile->name, sizeof(tile->name), text);
    }
    if (!tile->icon_from_card && (text = json_string(attrs, "icon"))) {
        copy_text(tile->icon, sizeof(tile->icon), text);
    }
    if ((text = json_string(attrs, "unit_of_measurement"))) {
        copy_text(tile->unit, sizeof(tile->unit), text);
    }
    if ((text = json_string(attrs, "device_class"))) {
        copy_text(tile->device_class, sizeof(tile->device_class), text);
    }

    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attrs, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int pct = (int)((brightness->valuedouble * 100.0) / 255.0 + 0.5);
        tile->brightness_pct = (int8_t)(pct < 1 ? 1 : pct > 100 ? 100 : pct);
    } else if (cJSON_IsNull(brightness)) {
        tile->brightness_pct = -1;
    }

    const cJSON *modes = cJSON_GetObjectItemCaseSensitive(attrs, "supported_color_modes");
    if (cJSON_IsArray(modes)) {
        bool dimmable = false;
        const cJSON *mode;
        cJSON_ArrayForEach(mode, modes) {
            if (cJSON_IsString(mode) && strcmp(mode->valuestring, "onoff") != 0) {
                dimmable = true;
            }
        }
        tile->dimmable = dimmable;
    }
}

static void remove_attributes(ha_dash_tile_t *tile, const cJSON *keys)
{
    const cJSON *key;
    cJSON_ArrayForEach(key, keys) {
        if (!cJSON_IsString(key)) {
            continue;
        }
        if (strcmp(key->valuestring, "brightness") == 0) {
            tile->brightness_pct = -1;
        } else if (strcmp(key->valuestring, "unit_of_measurement") == 0) {
            tile->unit[0] = '\0';
        } else if (strcmp(key->valuestring, "icon") == 0 && !tile->icon_from_card) {
            tile->icon[0] = '\0';
        }
    }
}

/* Compressed state: {"s": state, "a": {attributes}, ...}. */
static void apply_compressed(ha_dash_tile_t *tile, const cJSON *compressed)
{
    const char *state = json_string(compressed, "s");
    if (state) {
        copy_text(tile->state, sizeof(tile->state), state);
        tile->has_state = true;
    }
    apply_attributes(tile, cJSON_GetObjectItemCaseSensitive(compressed, "a"));
    if (tile->kind == HA_DASH_KIND_LIGHT && strcmp(tile->state, "on") != 0) {
        tile->brightness_pct = -1;
    }
}

typedef void (*tile_update_fn)(ha_dash_tile_t *tile, const cJSON *data);

static bool update_matching(ha_dash_snapshot_t *snap, const char *entity_id,
                            tile_update_fn update, const cJSON *data)
{
    bool changed = false;
    for (int i = 0; i < snap->tile_count; i++) {
        ha_dash_tile_t *tile = &snap->tiles[i];
        if (strcmp(tile->entity_id, entity_id) != 0) {
            continue;
        }
        ha_dash_tile_t before = *tile;
        update(tile, data);
        if (memcmp(&before, tile, sizeof(before)) != 0) {
            tile->revision = before.revision + 1;
            changed = true;
        }
    }
    return changed;
}

static void update_add(ha_dash_tile_t *tile, const cJSON *data)
{
    apply_compressed(tile, data);
}

static void update_change(ha_dash_tile_t *tile, const cJSON *data)
{
    remove_attributes(tile, cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(data, "-"), "a"));
    apply_compressed(tile, cJSON_GetObjectItemCaseSensitive(data, "+"));
}

static void update_remove(ha_dash_tile_t *tile, const cJSON *data)
{
    (void)data;
    tile->has_state = false;
    copy_text(tile->state, sizeof(tile->state), "unavailable");
    tile->brightness_pct = -1;
}

bool ha_dash_state_apply(ha_dash_snapshot_t *snap, const cJSON *event)
{
    bool changed = false;
    const cJSON *entry;

    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(event, "a")) {
        changed |= update_matching(snap, entry->string, update_add, entry);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(event, "c")) {
        changed |= update_matching(snap, entry->string, update_change, entry);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(event, "r")) {
        if (cJSON_IsString(entry)) {
            changed |= update_matching(snap, entry->valuestring, update_remove, NULL);
        }
    }
    return changed;
}

static const char *const SERVICE_TOGGLE_DOMAINS[] = {
    "light", "switch", "input_boolean", "fan", "automation", "siren",
    "humidifier", "cover", "media_player", "valve", NULL,
};

int ha_dash_build_action(const ha_dash_tile_t *tile, ha_dash_action_t action,
                         int value, int message_id, char *buf, size_t len)
{
    char domain[32];
    if (!ha_dash_entity_domain(tile->entity_id, domain, sizeof(domain))) {
        return 0;
    }

    const char *service = NULL;
    char data[48] = "";

    if (action == HA_DASH_ACTION_SET_LEVEL) {
        if (tile->kind != HA_DASH_KIND_LIGHT) {
            return 0;
        }
        if (value <= 0) {
            service = "turn_off";
        } else {
            service = "turn_on";
            snprintf(data, sizeof(data), ",\"service_data\":{\"brightness_pct\":%d}",
                     value > 100 ? 100 : value);
        }
    } else if (strcmp(domain, "lock") == 0) {
        service = strcmp(tile->state, "locked") == 0 ? "unlock" : "lock";
    } else if (strcmp(domain, "script") == 0 || strcmp(domain, "scene") == 0) {
        service = "turn_on";
    } else if (strcmp(domain, "button") == 0 || strcmp(domain, "input_button") == 0) {
        service = "press";
    } else if (domain_in(domain, SERVICE_TOGGLE_DOMAINS)) {
        service = "toggle";
    }

    if (service == NULL) {
        return 0;
    }
    int n = snprintf(buf, len,
                     "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\","
                     "\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"}%s}",
                     message_id, domain, service, tile->entity_id, data);
    return (n < 0 || (size_t)n >= len) ? -1 : n;
}
