#ifndef HA_DASH_H
#define HA_DASH_H

/* Home Assistant dashboard mirror.
 *
 * Connects to Home Assistant's WebSocket API with a long-lived access token,
 * reads the configured Lovelace dashboard, subscribes to the entities on it
 * and runs service calls for taps on the screen. Publishes
 * VIEW_EVENT_HA_DASH_CHANGED whenever the snapshot changes and handles
 * VIEW_EVENT_HA_DASH_ACTION from the screen. */

#include <stdbool.h>

#include "ha_dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int  ha_dash_init(void);

/* Reconnect with the stored configuration, e.g. after `setdash`. */
void ha_dash_reload(void);

/* Copy the current dashboard state. Returns false before ha_dash_init(). */
bool ha_dash_snapshot(ha_dash_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_H */
