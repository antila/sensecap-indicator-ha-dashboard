#include "ha_dash_present.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Font Awesome 5 code points rasterized into ui_font_dash_icons by
 * scripts/gen_dash_fonts.py. A code point missing from the font renders as
 * nothing, so add new ones to the generator first. */
enum {
    ICON_BULB, ICON_POWER, ICON_PLUG, ICON_TOGGLE, ICON_THERMO, ICON_DROP,
    ICON_LOCK, ICON_UNLOCK, ICON_DOOR_OPEN, ICON_DOOR_CLOSED, ICON_PLAY,
    ICON_BOLT, ICON_SUN, ICON_MOON, ICON_HOME, ICON_TV, ICON_MUSIC, ICON_SNOW,
    ICON_FIRE, ICON_WIND, ICON_USER, ICON_BELL, ICON_CLOUD, ICON_LEAF,
    ICON_CAR, ICON_VIDEO, ICON_COUCH, ICON_WINDOW, ICON_ARROWS, ICON_STAR,
    ICON_MAGIC, ICON_CODE, ICON_CHART, ICON_EYE, ICON_WALK, ICON_BATTERY,
    ICON_GAUGE, ICON_WIFI, ICON_CIRCLE, ICON_UTENSILS, ICON_BED, ICON_BATH,
    ICON_TREE, ICON_ROBOT, ICON_SMOG, ICON_SEEDLING, ICON_COFFEE, ICON_VOLUME,
    ICON_COG, ICON_BULLSEYE, ICON_GLOBE, ICON_DASHBOARD, ICON_MAP,
    ICON_COUNT,
};

#define GLYPH(cp) { (char)0xEF, (char)(0x80 | (((cp) >> 6) & 0x3F)), (char)(0x80 | ((cp) & 0x3F)), 0 }

static const char ICON_GLYPHS[ICON_COUNT][4] = {
    [ICON_BULB] = GLYPH(0xF0EB),       [ICON_POWER] = GLYPH(0xF011),
    [ICON_PLUG] = GLYPH(0xF1E6),       [ICON_TOGGLE] = GLYPH(0xF205),
    [ICON_THERMO] = GLYPH(0xF2C9),     [ICON_DROP] = GLYPH(0xF043),
    [ICON_LOCK] = GLYPH(0xF023),       [ICON_UNLOCK] = GLYPH(0xF3C1),
    [ICON_DOOR_OPEN] = GLYPH(0xF52B),  [ICON_DOOR_CLOSED] = GLYPH(0xF52A),
    [ICON_PLAY] = GLYPH(0xF04B),       [ICON_BOLT] = GLYPH(0xF0E7),
    [ICON_SUN] = GLYPH(0xF185),        [ICON_MOON] = GLYPH(0xF186),
    [ICON_HOME] = GLYPH(0xF015),       [ICON_TV] = GLYPH(0xF26C),
    [ICON_MUSIC] = GLYPH(0xF001),      [ICON_SNOW] = GLYPH(0xF2DC),
    [ICON_FIRE] = GLYPH(0xF06D),       [ICON_WIND] = GLYPH(0xF72E),
    [ICON_USER] = GLYPH(0xF007),       [ICON_BELL] = GLYPH(0xF0F3),
    [ICON_CLOUD] = GLYPH(0xF0C2),      [ICON_LEAF] = GLYPH(0xF06C),
    [ICON_CAR] = GLYPH(0xF1B9),        [ICON_VIDEO] = GLYPH(0xF03D),
    [ICON_COUCH] = GLYPH(0xF4B8),      [ICON_WINDOW] = GLYPH(0xF2D0),
    [ICON_ARROWS] = GLYPH(0xF338),     [ICON_STAR] = GLYPH(0xF005),
    [ICON_MAGIC] = GLYPH(0xF0D0),      [ICON_CODE] = GLYPH(0xF121),
    [ICON_CHART] = GLYPH(0xF201),      [ICON_EYE] = GLYPH(0xF06E),
    [ICON_WALK] = GLYPH(0xF554),       [ICON_BATTERY] = GLYPH(0xF241),
    [ICON_GAUGE] = GLYPH(0xF3FD),      [ICON_WIFI] = GLYPH(0xF1EB),
    [ICON_CIRCLE] = GLYPH(0xF111),     [ICON_UTENSILS] = GLYPH(0xF2E7),
    [ICON_BED] = GLYPH(0xF236),        [ICON_BATH] = GLYPH(0xF2CD),
    [ICON_TREE] = GLYPH(0xF1BB),       [ICON_ROBOT] = GLYPH(0xF544),
    [ICON_SMOG] = GLYPH(0xF75F),       [ICON_SEEDLING] = GLYPH(0xF4D8),
    [ICON_COFFEE] = GLYPH(0xF0F4),     [ICON_VOLUME] = GLYPH(0xF028),
    [ICON_COG] = GLYPH(0xF013),        [ICON_BULLSEYE] = GLYPH(0xF140),
    [ICON_GLOBE] = GLYPH(0xF0AC),
    [ICON_DASHBOARD] = GLYPH(0xF009), [ICON_MAP] = GLYPH(0xF279),
};

/* Matched as substrings of the mdi icon name, first match wins, so more
 * specific words come before the general ones they contain
 * ("lightning" before "light", "lock-open" before "lock"). */
static const struct {
    const char *word;
    int icon;
} MDI_KEYWORDS[] = {
    {"lightning", ICON_BOLT},     {"flash", ICON_BOLT},
    {"view-dashboard", ICON_DASHBOARD}, {"monitor-dashboard", ICON_DASHBOARD},
    {"map", ICON_MAP},
    {"thermo", ICON_THERMO},      {"temperature", ICON_THERMO},
    {"lock-open", ICON_UNLOCK},   {"unlock", ICON_UNLOCK},
    {"lock", ICON_LOCK},
    {"door-open", ICON_DOOR_OPEN}, {"door", ICON_DOOR_CLOSED},
    {"garage", ICON_CAR},
    {"blind", ICON_ARROWS},       {"shade", ICON_ARROWS},
    {"curtain", ICON_ARROWS},     {"shutter", ICON_ARROWS},
    {"window", ICON_WINDOW},
    {"light", ICON_BULB},         {"lamp", ICON_BULB},
    {"bulb", ICON_BULB},          {"led", ICON_BULB},
    {"chandelier", ICON_BULB},    {"sconce", ICON_BULB},
    {"power-plug", ICON_PLUG},    {"socket", ICON_PLUG},
    {"outlet", ICON_PLUG},        {"power", ICON_POWER},
    {"energy", ICON_BOLT},        {"meter-electric", ICON_BOLT},
    {"transmission-tower", ICON_BOLT},
    {"toggle", ICON_TOGGLE},
    {"fan", ICON_WIND},           {"wind", ICON_WIND},
    {"water", ICON_DROP},         {"humidity", ICON_DROP},
    {"drop", ICON_DROP},
    {"snow", ICON_SNOW},          {"air-conditioner", ICON_SNOW},
    {"hvac", ICON_SNOW},
    {"fire", ICON_FIRE},          {"radiator", ICON_FIRE},
    {"heat", ICON_FIRE},
    {"air-filter", ICON_SMOG},    {"air-purifier", ICON_SMOG},
    {"molecule", ICON_SMOG},      {"smog", ICON_SMOG},
    {"television", ICON_TV},      {"tv", ICON_TV},
    {"monitor", ICON_TV},
    {"speaker", ICON_MUSIC},      {"music", ICON_MUSIC},
    {"volume", ICON_VOLUME},
    {"sofa", ICON_COUCH},         {"couch", ICON_COUCH},
    {"bed", ICON_BED},
    {"shower", ICON_BATH},        {"bath", ICON_BATH},
    {"toilet", ICON_BATH},
    {"silverware", ICON_UTENSILS}, {"food", ICON_UTENSILS},
    {"stove", ICON_UTENSILS},     {"fridge", ICON_UTENSILS},
    {"kitchen", ICON_UTENSILS},   {"pot", ICON_UTENSILS},
    {"coffee", ICON_COFFEE},      {"kettle", ICON_COFFEE},
    {"battery", ICON_BATTERY},
    {"gauge", ICON_GAUGE},        {"speedometer", ICON_GAUGE},
    {"wifi", ICON_WIFI},          {"router", ICON_WIFI},
    {"access-point", ICON_WIFI},
    {"sprout", ICON_SEEDLING},    {"seed", ICON_SEEDLING},
    {"flower", ICON_LEAF},        {"leaf", ICON_LEAF},
    {"plant", ICON_LEAF},
    {"tree", ICON_TREE},          {"pine", ICON_TREE},
    {"bell", ICON_BELL},          {"alarm", ICON_BELL},
    {"cctv", ICON_VIDEO},         {"camera", ICON_VIDEO},
    {"video", ICON_VIDEO},        {"webcam", ICON_VIDEO},
    {"robot", ICON_ROBOT},        {"vacuum", ICON_ROBOT},
    {"palette", ICON_MAGIC},      {"creation", ICON_MAGIC},
    {"magic", ICON_MAGIC},        {"auto-fix", ICON_MAGIC},
    {"script", ICON_PLAY},        {"play", ICON_PLAY},
    {"star", ICON_STAR},
    {"weather-night", ICON_MOON}, {"moon", ICON_MOON},
    {"night", ICON_MOON},
    {"sun", ICON_SUN},            {"brightness", ICON_SUN},
    {"cloud", ICON_CLOUD},        {"weather", ICON_CLOUD},
    {"home", ICON_HOME},          {"house", ICON_HOME},
    {"car", ICON_CAR},
    {"account", ICON_USER},       {"human", ICON_USER},
    {"person", ICON_USER},
    {"motion", ICON_WALK},        {"walk", ICON_WALK},
    {"run", ICON_WALK},
    {"eye", ICON_EYE},
    {"chart", ICON_CHART},        {"graph", ICON_CHART},
    {"cog", ICON_COG},            {"gear", ICON_COG},
    {"earth", ICON_GLOBE},        {"web", ICON_GLOBE},
    {"globe", ICON_GLOBE},
};

bool ha_dash_entity_domain(const char *entity_id, char *domain, size_t len)
{
    const char *dot = entity_id ? strchr(entity_id, '.') : NULL;
    if (dot == NULL || len == 0) {
        return false;
    }
    size_t n = (size_t)(dot - entity_id);
    if (n >= len) {
        n = len - 1;
    }
    memcpy(domain, entity_id, n);
    domain[n] = '\0';
    return true;
}

static bool is_state(const ha_dash_tile_t *tile, const char *state)
{
    return strcmp(tile->state, state) == 0;
}

static bool class_is(const ha_dash_tile_t *tile, const char *const *classes)
{
    for (; *classes; classes++) {
        if (strcmp(tile->device_class, *classes) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const OPENING_CLASSES[] = {"door", "garage_door", "opening", NULL};
static const char *const PRESENCE_CLASSES[] = {"motion", "occupancy", "presence", NULL};
static const char *const AIR_CLASSES[] = {
    "carbon_dioxide", "pm25", "pm10", "pm1", "aqi", "volatile_organic_compounds",
    "volatile_organic_compounds_parts", "nitrogen_dioxide", NULL,
};
static const char *const ELECTRIC_CLASSES[] = {
    "power", "energy", "current", "voltage", "apparent_power", "reactive_power", NULL,
};

static int icon_for_domain(const ha_dash_tile_t *tile, const char *domain)
{
    bool active = ha_dash_tile_is_active(tile);

    if (strcmp(domain, "light") == 0) return ICON_BULB;
    if (strcmp(domain, "switch") == 0) {
        return strcmp(tile->device_class, "outlet") == 0 ? ICON_PLUG : ICON_POWER;
    }
    if (strcmp(domain, "input_boolean") == 0) return ICON_TOGGLE;
    if (strcmp(domain, "fan") == 0) return ICON_WIND;
    if (strcmp(domain, "cover") == 0) {
        return strcmp(tile->device_class, "garage") == 0 ? ICON_CAR : ICON_ARROWS;
    }
    if (strcmp(domain, "lock") == 0) return active ? ICON_UNLOCK : ICON_LOCK;
    if (strcmp(domain, "script") == 0) return ICON_PLAY;
    if (strcmp(domain, "scene") == 0) return ICON_MAGIC;
    if (strcmp(domain, "automation") == 0) return ICON_CODE;
    if (strcmp(domain, "button") == 0 || strcmp(domain, "input_button") == 0) return ICON_BULLSEYE;
    if (strcmp(domain, "media_player") == 0) return ICON_TV;
    if (strcmp(domain, "climate") == 0 || strcmp(domain, "water_heater") == 0) return ICON_THERMO;
    if (strcmp(domain, "weather") == 0) return ICON_CLOUD;
    if (strcmp(domain, "person") == 0 || strcmp(domain, "device_tracker") == 0) return ICON_USER;
    if (strcmp(domain, "camera") == 0) return ICON_VIDEO;
    if (strcmp(domain, "vacuum") == 0) return ICON_ROBOT;
    if (strcmp(domain, "sun") == 0) return ICON_SUN;
    if (strcmp(domain, "siren") == 0) return ICON_BELL;

    if (strcmp(domain, "binary_sensor") == 0) {
        if (class_is(tile, OPENING_CLASSES)) return active ? ICON_DOOR_OPEN : ICON_DOOR_CLOSED;
        if (strcmp(tile->device_class, "window") == 0) return ICON_WINDOW;
        if (class_is(tile, PRESENCE_CLASSES)) return ICON_WALK;
        if (strcmp(tile->device_class, "smoke") == 0 || strcmp(tile->device_class, "heat") == 0) return ICON_FIRE;
        if (strcmp(tile->device_class, "moisture") == 0) return ICON_DROP;
        if (strcmp(tile->device_class, "battery") == 0) return ICON_BATTERY;
        if (strcmp(tile->device_class, "power") == 0 || strcmp(tile->device_class, "plug") == 0) return ICON_PLUG;
        return ICON_EYE;
    }

    if (strcmp(tile->device_class, "temperature") == 0) return ICON_THERMO;
    if (strcmp(tile->device_class, "humidity") == 0 || strcmp(tile->device_class, "moisture") == 0) return ICON_DROP;
    if (class_is(tile, ELECTRIC_CLASSES)) return ICON_BOLT;
    if (strcmp(tile->device_class, "battery") == 0) return ICON_BATTERY;
    if (strcmp(tile->device_class, "illuminance") == 0) return ICON_SUN;
    if (class_is(tile, AIR_CLASSES)) return ICON_SMOG;
    if (strcmp(tile->device_class, "pressure") == 0) return ICON_GAUGE;
    if (strcmp(domain, "sensor") == 0) return ICON_CHART;
    return ICON_CIRCLE;
}

const char *ha_dash_icon_glyph(const ha_dash_tile_t *tile)
{
    const char *icon = tile->icon;
    if (strncmp(icon, "mdi:", 4) == 0) {
        icon += 4;
    }
    if (icon[0] != '\0') {
        for (size_t i = 0; i < sizeof(MDI_KEYWORDS) / sizeof(MDI_KEYWORDS[0]); i++) {
            if (strstr(icon, MDI_KEYWORDS[i].word)) {
                return ICON_GLYPHS[MDI_KEYWORDS[i].icon];
            }
        }
    }
    char domain[32] = "";
    ha_dash_entity_domain(tile->entity_id, domain, sizeof(domain));
    return ICON_GLYPHS[icon_for_domain(tile, domain)];
}

static const char *const ACTIVE_STATES[] = {
    "on", "open", "opening", "closing", "unlocked", "unlocking", "open",
    "playing", "home", "cleaning", "heat", "cool", "heat_cool", "auto", "dry",
    "fan_only", NULL,
};

bool ha_dash_tile_is_active(const ha_dash_tile_t *tile)
{
    if (!tile->has_state) {
        return false;
    }
    char domain[32] = "";
    ha_dash_entity_domain(tile->entity_id, domain, sizeof(domain));
    if (tile->kind == HA_DASH_KIND_SENSOR &&
        strcmp(domain, "binary_sensor") != 0 && strcmp(domain, "climate") != 0) {
        return false;
    }
    for (const char *const *s = ACTIVE_STATES; *s; s++) {
        if (is_state(tile, *s)) {
            return true;
        }
    }
    return false;
}

bool ha_dash_tile_is_unavailable(const ha_dash_tile_t *tile)
{
    return !tile->has_state || is_state(tile, "unavailable");
}

static bool format_number(const char *text, char *buf, size_t len)
{
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    if (strchr(text, '.') == NULL) {
        snprintf(buf, len, "%s", text);
    } else {
        double magnitude = fabs(value);
        snprintf(buf, len, "%.*f", magnitude >= 100 ? 0 : magnitude >= 1 ? 1 : 2, value);
    }
    return true;
}

static void humanize(const char *state, char *buf, size_t len)
{
    snprintf(buf, len, "%s", state);
    for (char *p = buf; *p; p++) {
        if (*p == '_') {
            *p = ' ';
        }
    }
    if (buf[0] >= 'a' && buf[0] <= 'z') {
        buf[0] = (char)(buf[0] - 'a' + 'A');
    }
}

void ha_dash_format_state(const ha_dash_tile_t *tile, char *buf, size_t len)
{
    char domain[32] = "";
    ha_dash_entity_domain(tile->entity_id, domain, sizeof(domain));

    if (!tile->has_state) {
        snprintf(buf, len, "%s", is_state(tile, "unavailable") ? "Unavailable" : "...");
        return;
    }
    if (is_state(tile, "unavailable") || is_state(tile, "unknown")) {
        humanize(tile->state, buf, len);
        return;
    }

    if (tile->kind == HA_DASH_KIND_BUTTON) {
        /* Scene and button states are last-triggered timestamps. */
        snprintf(buf, len, "%s", strcmp(domain, "script") == 0 && is_state(tile, "on") ? "Running" : "");
        return;
    }

    if (tile->kind == HA_DASH_KIND_LIGHT && is_state(tile, "on") && tile->brightness_pct >= 0) {
        snprintf(buf, len, "On \xC2\xB7 %d%%", tile->brightness_pct);
        return;
    }

    if (strcmp(domain, "binary_sensor") == 0) {
        bool on = is_state(tile, "on");
        const char *text = on ? "On" : "Off";
        if (class_is(tile, OPENING_CLASSES) || strcmp(tile->device_class, "window") == 0) {
            text = on ? "Open" : "Closed";
        } else if (class_is(tile, PRESENCE_CLASSES)) {
            text = on ? "Detected" : "Clear";
        } else if (strcmp(tile->device_class, "moisture") == 0) {
            text = on ? "Wet" : "Dry";
        } else if (strcmp(tile->device_class, "battery") == 0) {
            text = on ? "Low" : "Normal";
        }
        snprintf(buf, len, "%s", text);
        return;
    }

    if (tile->kind == HA_DASH_KIND_SENSOR && format_number(tile->state, buf, len)) {
        return;
    }
    humanize(tile->state, buf, len);
}
