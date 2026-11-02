#include "ha_dash_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "ha_dash_layout.h"

static const char *TAG = "ha-dash-auth";

#define AUTH_TIMEOUT_MS  10000
#define AUTH_RESPONSE    2048
#define AUTH_URL_LEN     256
#define AUTH_FORM_LEN    768

typedef struct {
    int status;
    cJSON *json; /* NULL when the body was not JSON */
} auth_response_t;

static void set_error(char *error, size_t len, const char *text)
{
    if (error && len) {
        snprintf(error, len, "%s", text);
    }
}

/* POST body to base_url + path and parse the JSON reply. */
static esp_err_t auth_post(const char *ws_url, const char *path, const char *content_type,
                           const char *body, auth_response_t *out)
{
    char url[AUTH_URL_LEN];
    char base[AUTH_URL_LEN - 32];
    out->status = 0;
    out->json = NULL;
    if (!ha_dash_http_base(ws_url, base, sizeof(base))) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(url, sizeof(url), "%s%s", base, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = AUTH_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* only used for https */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", content_type);

    char *reply = heap_caps_calloc(1, AUTH_RESPONSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int body_len = (int)strlen(body);
    esp_err_t err = reply ? esp_http_client_open(client, body_len) : ESP_ERR_NO_MEM;
    if (err == ESP_OK && esp_http_client_write(client, body, body_len) != body_len) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        int total = 0;
        int n;
        while (total < AUTH_RESPONSE - 1 &&
               (n = esp_http_client_read(client, reply + total, AUTH_RESPONSE - 1 - total)) > 0) {
            total += n;
        }
        out->status = esp_http_client_get_status_code(client);
        out->json = cJSON_Parse(reply);
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
    }

    free(reply);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static const char *json_text(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* POST a JSON object (consumed) to a login-flow endpoint. */
static esp_err_t post_json(const char *ws_url, const char *path, cJSON *body, auth_response_t *out)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = auth_post(ws_url, path, "application/json", text, out);
    /* The login step carries the password; don't leave it in freed memory. */
    memset(text, 0, strlen(text));
    cJSON_free(text);
    return err;
}

/* /auth/token with a form body built from key/value pairs. */
static esp_err_t post_token(const char *ws_url, const char *const *pairs, auth_response_t *out)
{
    char form[AUTH_FORM_LEN];
    size_t used = 0;
    for (int i = 0; pairs[i]; i += 2) {
        int n = snprintf(form + used, sizeof(form) - used, "%s%s=", used ? "&" : "", pairs[i]);
        if (n < 0 || (size_t)n >= sizeof(form) - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        used += (size_t)n;
        n = ha_dash_url_encode(pairs[i + 1], form + used, sizeof(form) - used);
        if (n < 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        used += (size_t)n;
    }
    esp_err_t err = auth_post(ws_url, "/auth/token", "application/x-www-form-urlencoded", form, out);
    memset(form, 0, sizeof(form));
    return err;
}

static esp_err_t network_error(esp_err_t err, char *error, size_t len)
{
    set_error(error, len, "Can't reach Home Assistant");
    return err == ESP_OK ? ESP_FAIL : err;
}

esp_err_t ha_dash_auth_login(const char *ws_url, const char *username, const char *password,
                             char *refresh_token, size_t refresh_len,
                             char *error, size_t error_len)
{
    auth_response_t r;

    /* 1. Start the login flow with the built-in username/password provider. */
    cJSON *start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "client_id", HA_DASH_CLIENT_ID);
    cJSON *handler = cJSON_AddArrayToObject(start, "handler");
    cJSON_AddItemToArray(handler, cJSON_CreateString("homeassistant"));
    cJSON_AddItemToArray(handler, cJSON_CreateNull());
    cJSON_AddStringToObject(start, "redirect_uri", HA_DASH_CLIENT_ID "auth");
    esp_err_t err = post_json(ws_url, "/auth/login_flow", start, &r);
    const char *flow_id = json_text(r.json, "flow_id");
    if (err != ESP_OK || r.status != 200 || flow_id == NULL) {
        ESP_LOGW(TAG, "login flow start: HTTP %d", r.status);
        cJSON_Delete(r.json);
        return network_error(err, error, error_len);
    }
    char path[96];
    snprintf(path, sizeof(path), "/auth/login_flow/%s", flow_id);
    cJSON_Delete(r.json);

    /* 2. Submit the credentials. */
    cJSON *step = cJSON_CreateObject();
    cJSON_AddStringToObject(step, "client_id", HA_DASH_CLIENT_ID);
    cJSON_AddStringToObject(step, "username", username);
    cJSON_AddStringToObject(step, "password", password);
    err = post_json(ws_url, path, step, &r);
    if (err != ESP_OK || r.json == NULL) {
        cJSON_Delete(r.json);
        return network_error(err, error, error_len);
    }
    const char *type = json_text(r.json, "type");
    const char *step_id = json_text(r.json, "step_id");
    const char *code = json_text(r.json, "result");
    if (type && strcmp(type, "create_entry") == 0 && code) {
        char code_copy[96];
        snprintf(code_copy, sizeof(code_copy), "%s", code);
        cJSON_Delete(r.json);

        /* 3. Exchange the authorization code for tokens. */
        const char *pairs[] = {"grant_type", "authorization_code", "code", code_copy,
                               "client_id", HA_DASH_CLIENT_ID, NULL};
        err = post_token(ws_url, pairs, &r);
        const char *refresh = json_text(r.json, "refresh_token");
        if (err == ESP_OK && r.status == 200 && refresh && strlen(refresh) < refresh_len) {
            snprintf(refresh_token, refresh_len, "%s", refresh);
            cJSON_Delete(r.json);
            ESP_LOGI(TAG, "signed in as %s", username);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "token exchange: HTTP %d", r.status);
        cJSON_Delete(r.json);
        return network_error(err, error, error_len);
    }

    if (step_id && strcmp(step_id, "mfa") == 0) {
        set_error(error, error_len, "Two-factor login isn't supported; use a token");
    } else if (type && strcmp(type, "abort") == 0) {
        set_error(error, error_len, "Home Assistant refused the login");
    } else {
        set_error(error, error_len, "Wrong username or password");
    }
    cJSON_Delete(r.json);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t ha_dash_auth_refresh(const char *ws_url, const char *refresh_token,
                               char *access_token, size_t access_len, int *expires_in,
                               char *error, size_t error_len)
{
    auth_response_t r;
    const char *pairs[] = {"grant_type", "refresh_token", "refresh_token", refresh_token,
                           "client_id", HA_DASH_CLIENT_ID, NULL};
    esp_err_t err = post_token(ws_url, pairs, &r);
    if (err != ESP_OK) {
        cJSON_Delete(r.json);
        return network_error(err, error, error_len);
    }

    const char *access = json_text(r.json, "access_token");
    if (r.status == 200 && access && strlen(access) < access_len) {
        snprintf(access_token, access_len, "%s", access);
        const cJSON *expires = cJSON_GetObjectItemCaseSensitive(r.json, "expires_in");
        if (expires_in) {
            *expires_in = cJSON_IsNumber(expires) ? expires->valueint : 1800;
        }
        cJSON_Delete(r.json);
        return ESP_OK;
    }

    const char *reason = json_text(r.json, "error");
    ESP_LOGW(TAG, "token refresh: HTTP %d %s", r.status, reason ? reason : "");
    bool rejected = r.status == 400 || r.status == 403 || (reason && strcmp(reason, "invalid_grant") == 0);
    cJSON_Delete(r.json);
    if (rejected) {
        set_error(error, error_len, "Signed out; sign in again");
        return ESP_ERR_INVALID_STATE;
    }
    return network_error(ESP_FAIL, error, error_len);
}
