#ifndef HA_DASH_TYPES_H
#define HA_DASH_TYPES_H

/* Plain data shared by the Home Assistant dashboard model and screen.
 *
 * The model (ha_dash.c) owns the authoritative copy and hands the screen a
 * snapshot; nothing here depends on ESP-IDF or LVGL, so the parser and
 * presentation helpers can be tested on the host. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HA_DASH_MAX_TILES     32
#define HA_DASH_ENTITY_ID_LEN 64
#define HA_DASH_NAME_LEN      48
#define HA_DASH_ICON_LEN      40
#define HA_DASH_STATE_LEN     32
#define HA_DASH_UNIT_LEN      16
#define HA_DASH_CLASS_LEN     24
#define HA_DASH_TITLE_LEN     48
#define HA_DASH_MESSAGE_LEN   64

typedef enum {
    HA_DASH_KIND_SENSOR = 0, /* read-only value */
    HA_DASH_KIND_TOGGLE,     /* switch, input_boolean, fan, cover, lock, ... */
    HA_DASH_KIND_LIGHT,      /* toggle, plus a brightness slider when dimmable */
    HA_DASH_KIND_BUTTON,     /* script, scene, button, input_button */
    HA_DASH_KIND_HEADING,    /* section title, no entity */
} ha_dash_kind_t;

typedef enum {
    HA_DASH_STATUS_UNCONFIGURED = 0,
    HA_DASH_STATUS_WAITING_NETWORK,
    HA_DASH_STATUS_CONNECTING,
    HA_DASH_STATUS_AUTH_FAILED,
    HA_DASH_STATUS_LOADING,
    HA_DASH_STATUS_READY,
    HA_DASH_STATUS_ERROR,
} ha_dash_status_t;

typedef enum {
    HA_DASH_ACTION_TAP = 0,       /* toggle / press, depending on the tile kind */
    HA_DASH_ACTION_SET_LEVEL,     /* light brightness in percent (0 turns it off) */
} ha_dash_action_t;

typedef struct {
    ha_dash_kind_t kind;
    bool wide;          /* spans the full screen width */
    bool tap_enabled;   /* false when the card sets tap_action: none */
    bool name_from_card;
    bool icon_from_card;
    char entity_id[HA_DASH_ENTITY_ID_LEN];
    char name[HA_DASH_NAME_LEN];
    char icon[HA_DASH_ICON_LEN]; /* "mdi:..." or empty */

    /* Live state from Home Assistant. */
    bool has_state;
    bool dimmable;
    int8_t brightness_pct; /* -1 when unknown or the light is off */
    char state[HA_DASH_STATE_LEN];
    char unit[HA_DASH_UNIT_LEN];
    char device_class[HA_DASH_CLASS_LEN];
    uint32_t revision;     /* bumped on every change to this tile */
} ha_dash_tile_t;

typedef struct {
    ha_dash_status_t status;
    char message[HA_DASH_MESSAGE_LEN];
    char title[HA_DASH_TITLE_LEN];
    uint32_t layout_generation; /* bumped whenever the tile list is rebuilt */
    uint8_t tile_count;
    ha_dash_tile_t tiles[HA_DASH_MAX_TILES];
} ha_dash_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_TYPES_H */
