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

/* Sign in with a username and password (asynchronous). On success the
 * address, username and resulting refresh token are saved and the dashboard
 * reconnects; the outcome is reported through the snapshot's login_* fields.
 * An empty address keeps the stored one. Called directly rather than through
 * a view event so the password is not copied into the event queue. */
void ha_dash_login(const char *address, const char *username, const char *password);

/* Copy the current dashboard state. Returns false before ha_dash_init(). */
bool ha_dash_snapshot(ha_dash_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_H */
