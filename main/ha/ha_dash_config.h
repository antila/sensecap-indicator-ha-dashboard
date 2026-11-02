#ifndef HA_DASH_CONFIG_H
#define HA_DASH_CONFIG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection settings for the Home Assistant dashboard screen. Stored as
 * separate NVS keys, independent of the MQTT ha_cfg_interface blob. */

/* Until one is picked on the device, show Home Assistant's default dashboard. */
#define HA_DASH_DEFAULT_DASHBOARD ""

/* Two ways to sign in. A username/password login stores only the resulting
 * refresh token (never the password); a long-lived token is used as is. When
 * both are present the refresh token wins. */
typedef struct {
    char url[128];           /* ws://host:8123/api/websocket */
    char token[256];         /* long-lived access token */
    char refresh_token[192]; /* from a username/password login */
    char username[64];       /* last login, to pre-fill the sign-in form */
    char dashboard[64];      /* dashboard url_path; empty = default dashboard */
} ha_dash_config_t;

/* Fills cfg (missing fields are empty, dashboard defaults to
 * HA_DASH_DEFAULT_DASHBOARD). Returns ESP_ERR_NOT_FOUND when the URL or a
 * credential has not been set yet. */
esp_err_t ha_dash_config_get(ha_dash_config_t *cfg);
esp_err_t ha_dash_config_set(const ha_dash_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_CONFIG_H */
