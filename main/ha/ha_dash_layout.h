#ifndef HA_DASH_LAYOUT_H
#define HA_DASH_LAYOUT_H

/* Home Assistant WebSocket payload handling for the dashboard screen.
 *
 * Pure functions over cJSON: turn a Lovelace dashboard config into tiles,
 * apply `subscribe_entities` updates to those tiles, and build the
 * `call_service` messages a tap sends back. No I/O and no locking; the model
 * calls these while holding its own lock. */

#include <stddef.h>

#include "cJSON.h"
#include "ha_dash_present.h"
#include "ha_dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill out->title, out->tile_count and out->tiles from the `result` object of
 * a `lovelace/config` response. Only the first view is used. Returns the
 * number of tiles, or -1 when the config has no usable view. Live state
 * fields are reset; status, message and layout_generation are left alone. */
int ha_dash_layout_parse(const cJSON *config, ha_dash_snapshot_t *out);

/* Fill out[] from the `result` array of `lovelace/dashboards/list`, followed
 * by the default dashboard (url_path ""). Returns the number of entries. */
int ha_dash_parse_dashboards(const cJSON *result, ha_dash_board_t *out, int max);

/* Write a JSON array of the unique entity ids in the layout into buf.
 * Returns the string length, or -1 if buf is too small. */
int ha_dash_layout_entity_ids_json(const ha_dash_snapshot_t *snap, char *buf, size_t len);

/* Apply the `event` object of a `subscribe_entities` message ("a" adds,
 * "c" changes, "r" removals). Returns true if any tile changed. */
bool ha_dash_state_apply(ha_dash_snapshot_t *snap, const cJSON *event);

/* Build the call_service message for an action on a tile. Returns the string
 * length, 0 when the tile has nothing to do for this action, or -1 if buf is
 * too small. */
int ha_dash_build_action(const ha_dash_tile_t *tile, ha_dash_action_t action,
                         int value, int message_id, char *buf, size_t len);

/* Turn "192.168.1.5", "host:8123", "http(s)://host[:port]" or a ws(s):// URL
 * into Home Assistant's WebSocket API URL. A bare host gets port 8123; an
 * explicit scheme keeps its default port. Returns false if out is too small
 * or the input is empty. */
bool ha_dash_websocket_url(const char *input, char *out, size_t len);

/* "ws://host:8123/api/websocket" -> "http://host:8123" (wss -> https), the
 * base for the /auth endpoints. Returns false if out is too small. */
bool ha_dash_http_base(const char *ws_url, char *out, size_t len);

/* "ws://host:8123/api/websocket" -> "host:8123", for display and for
 * pre-filling the sign-in form. */
void ha_dash_server_label(const char *ws_url, char *out, size_t len);

/* Percent-encode text for an application/x-www-form-urlencoded body.
 * Returns the encoded length, or -1 if out is too small. */
int ha_dash_url_encode(const char *text, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_LAYOUT_H */
