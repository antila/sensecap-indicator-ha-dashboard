#ifndef HA_DASH_AUTH_H
#define HA_DASH_AUTH_H

/* Home Assistant username/password sign-in over HTTP.
 *
 * The WebSocket API only accepts access tokens, so a login runs Home
 * Assistant's login flow (/auth/login_flow) once and exchanges the result
 * for a refresh token (/auth/token). Short-lived access tokens are then
 * minted from the refresh token before each connection. Blocking; call from
 * the dashboard worker task only. */

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Identifies the device to Home Assistant. Must be a URL with a host name,
 * not an IP address; it is never fetched. */
#define HA_DASH_CLIENT_ID "http://sensecap-indicator.local/"

/* Sign in. On success fills refresh_token. On failure error holds a short,
 * user-facing reason ("Wrong username or password", ...). */
esp_err_t ha_dash_auth_login(const char *ws_url, const char *username, const char *password,
                             char *refresh_token, size_t refresh_len,
                             char *error, size_t error_len);

/* Mint an access token from a refresh token. Returns ESP_ERR_INVALID_STATE
 * when Home Assistant rejected the refresh token (sign in again), other
 * errors for network problems worth retrying. */
esp_err_t ha_dash_auth_refresh(const char *ws_url, const char *refresh_token,
                               char *access_token, size_t access_len, int *expires_in,
                               char *error, size_t error_len);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_AUTH_H */
