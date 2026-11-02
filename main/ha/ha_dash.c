#include "ha_dash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ha_dash_auth.h"
#include "ha_dash_config.h"
#include "ha_dash_layout.h"
#include "view_event.h"

static const char *TAG = "ha-dash";

#define DASH_QUEUE_LEN       16
#define DASH_TASK_STACK      8192
#define DASH_MAX_MESSAGE     (192 * 1024)
#define DASH_SEND_TIMEOUT_MS 5000
#define DASH_ACTION_BUF      384

typedef enum {
    DASH_ITEM_NETWORK_UP,
    DASH_ITEM_NETWORK_DOWN,
    DASH_ITEM_RELOAD,
    DASH_ITEM_CONNECTED,
    DASH_ITEM_DISCONNECTED,
    DASH_ITEM_MESSAGE,
    DASH_ITEM_ACTION,
    DASH_ITEM_LOGIN,
} dash_item_kind_t;

typedef struct {
    char address[128];
    char username[64];
    char password[64];
} dash_login_t;

typedef struct {
    dash_item_kind_t kind;
    char *text;          /* DASH_ITEM_MESSAGE, owned by the receiver */
    dash_login_t *login; /* DASH_ITEM_LOGIN, owned by the receiver */
    struct view_data_ha_dash_action action;
} dash_item_t;

/* Shared with the screen through ha_dash_snapshot(); guarded by s_lock. */
static SemaphoreHandle_t s_lock;
static ha_dash_snapshot_t *s_snap;

static QueueHandle_t s_queue;

/* Worker-task state. */
static esp_websocket_client_handle_t s_client;
static ha_dash_config_t s_cfg;
static bool s_network;
static bool s_authed;
static int s_next_id;
static int s_config_request_id;
static int s_entities_sub_id;
static int s_lovelace_sub_id;
static int s_boards_request_id;

/* Access token minted from the refresh token of a username/password login. */
#define DASH_ACCESS_TOKEN_LEN 512
#define DASH_TOKEN_MARGIN_US  (60LL * 1000 * 1000)
static char s_access_token[DASH_ACCESS_TOKEN_LEN];
static int64_t s_access_expires_us;
static int s_auth_failures;

/* WebSocket-task state: reassembly of a message split over several events. */
static char *s_rx_buf;
static int s_rx_len;

static void *dash_alloc(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, size);
}

/* cJSON makes one small allocation per node. A large dashboard has thousands,
 * and small allocations would otherwise come from internal RAM and starve the
 * drivers (I2C, Wi-Fi), so parse into PSRAM. */
static void *json_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void notify_changed(void)
{
    struct view_data_ha_dash_changed changed;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed.layout_generation = s_snap->layout_generation;
    xSemaphoreGive(s_lock);
    view_event_publish_state(VIEW_EVENT_HA_DASH_CHANGED, &changed, sizeof(changed));
}

/* Copy server and username from s_cfg into the snapshot (no notify). */
static void snapshot_connection(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_snap->dashboard, s_cfg.dashboard, sizeof(s_snap->dashboard));
    ha_dash_server_label(s_cfg.url, s_snap->server, sizeof(s_snap->server));
    strlcpy(s_snap->username, s_cfg.refresh_token[0] ? s_cfg.username : "", sizeof(s_snap->username));
    xSemaphoreGive(s_lock);
}

static void set_status(ha_dash_status_t status, const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap->status = status;
    strlcpy(s_snap->message, message ? message : "", sizeof(s_snap->message));
    xSemaphoreGive(s_lock);
    notify_changed();
}

static bool post_item(const dash_item_t *item, TickType_t wait)
{
    if (s_queue == NULL || xQueueSend(s_queue, item, wait) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped item %d", (int)item->kind);
        return false;
    }
    return true;
}

/* ── WebSocket task ─────────────────────────────────────────────────────── */

static void rx_reset(void)
{
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = 0;
}

static void on_ws_data(const esp_websocket_event_data_t *data)
{
    if (data->op_code != 0x1 && data->op_code != 0x0) {
        return; /* only text frames carry API messages */
    }
    if (data->payload_offset == 0) {
        rx_reset();
        if (data->payload_len <= 0 || data->payload_len > DASH_MAX_MESSAGE) {
            if (data->payload_len > DASH_MAX_MESSAGE) {
                ESP_LOGW(TAG, "dropping %d-byte message", data->payload_len);
            }
            return;
        }
        s_rx_buf = dash_alloc((size_t)data->payload_len + 1);
        s_rx_len = data->payload_len;
        if (s_rx_buf == NULL) {
            ESP_LOGE(TAG, "no memory for %d-byte message", data->payload_len);
            return;
        }
    }
    if (s_rx_buf == NULL || data->payload_offset + data->data_len > s_rx_len) {
        return;
    }
    memcpy(s_rx_buf + data->payload_offset, data->data_ptr, (size_t)data->data_len);
    if (data->payload_offset + data->data_len < s_rx_len) {
        return; /* more to come */
    }

    dash_item_t item = {.kind = DASH_ITEM_MESSAGE, .text = s_rx_buf};
    s_rx_buf = NULL;
    s_rx_len = 0;
    if (!post_item(&item, pdMS_TO_TICKS(1000))) {
        free(item.text);
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_websocket_event_data_t *data = event_data;
    dash_item_t item = {0};

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            item.kind = DASH_ITEM_CONNECTED;
            post_item(&item, pdMS_TO_TICKS(1000));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            rx_reset();
            item.kind = DASH_ITEM_DISCONNECTED;
            post_item(&item, pdMS_TO_TICKS(1000));
            break;
        case WEBSOCKET_EVENT_DATA:
            on_ws_data(data);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "websocket error: handshake status %d, socket errno %d",
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_transport_sock_errno);
            break;
        default:
            break;
    }
}

/* ── Worker task ────────────────────────────────────────────────────────── */

static bool send_text(const char *text)
{
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        return false;
    }
    int len = (int)strlen(text);
    if (esp_websocket_client_send_text(s_client, text, len, pdMS_TO_TICKS(DASH_SEND_TIMEOUT_MS)) < 0) {
        ESP_LOGW(TAG, "send failed");
        return false;
    }
    return true;
}

static void client_stop(void);

/* With a username/password login, mint a fresh access token when needed.
 * Returns false when there is no usable token. */
static bool ensure_access_token(void)
{
    if (s_cfg.refresh_token[0] == '\0') {
        return true; /* long-lived token */
    }
    if (s_access_token[0] && esp_timer_get_time() < s_access_expires_us - DASH_TOKEN_MARGIN_US) {
        return true;
    }

    char error[HA_DASH_MESSAGE_LEN];
    int expires_in = 0;
    esp_err_t err = ha_dash_auth_refresh(s_cfg.url, s_cfg.refresh_token, s_access_token,
                                         sizeof(s_access_token), &expires_in, error, sizeof(error));
    if (err == ESP_OK) {
        s_access_expires_us = esp_timer_get_time() + (int64_t)expires_in * 1000 * 1000;
        return true;
    }
    s_access_token[0] = '\0';
    if (err == ESP_ERR_INVALID_STATE) {
        client_stop(); /* the refresh token was revoked; only a new login helps */
        set_status(HA_DASH_STATUS_AUTH_FAILED, error);
    } else {
        set_status(HA_DASH_STATUS_CONNECTING, error); /* retried on the next connect */
    }
    return false;
}

static void send_auth(void)
{
    if (!ensure_access_token()) {
        return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "auth");
    cJSON_AddStringToObject(msg, "access_token", s_cfg.refresh_token[0] ? s_access_token : s_cfg.token);
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (text) {
        send_text(text);
        cJSON_free(text);
    }
}

static void request_config(void)
{
    char msg[160];
    s_config_request_id = ++s_next_id;
    if (s_cfg.dashboard[0]) {
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"lovelace/config\",\"url_path\":\"%s\"}",
                 s_config_request_id, s_cfg.dashboard);
    } else {
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"lovelace/config\",\"url_path\":null}",
                 s_config_request_id);
    }
    send_text(msg);
}

static void request_dashboards(void)
{
    char msg[64];
    s_boards_request_id = ++s_next_id;
    snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"lovelace/dashboards/list\"}",
             s_boards_request_id);
    send_text(msg);
}

static void subscribe_lovelace_updates(void)
{
    char msg[96];
    s_lovelace_sub_id = ++s_next_id;
    snprintf(msg, sizeof(msg),
             "{\"id\":%d,\"type\":\"subscribe_events\",\"event_type\":\"lovelace_updated\"}",
             s_lovelace_sub_id);
    send_text(msg);
}

static void subscribe_entities(void)
{
    if (s_entities_sub_id) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"unsubscribe_events\",\"subscription\":%d}",
                 ++s_next_id, s_entities_sub_id);
        send_text(msg);
        s_entities_sub_id = 0;
    }

    size_t len = HA_DASH_MAX_TILES * (HA_DASH_ENTITY_ID_LEN + 4) + 96;
    char *msg = dash_alloc(len);
    if (msg == NULL) {
        return;
    }
    int id = ++s_next_id;
    int n = snprintf(msg, len, "{\"id\":%d,\"type\":\"subscribe_entities\",\"entity_ids\":", id);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ids = ha_dash_layout_entity_ids_json(s_snap, msg + n, len - (size_t)n - 1);
    xSemaphoreGive(s_lock);

    if (ids > 2) { /* not just "[]" */
        strcat(msg, "}");
        if (send_text(msg)) {
            s_entities_sub_id = id;
        }
    }
    free(msg);
}

/* Carry live state over from the previous layout so an edited dashboard
 * doesn't flash "..." until the new subscription's first update arrives. */
static void carry_over_state(ha_dash_tile_t *tile, const ha_dash_snapshot_t *old)
{
    for (int i = 0; i < old->tile_count; i++) {
        const ha_dash_tile_t *prev = &old->tiles[i];
        if (tile->entity_id[0] == '\0' || strcmp(prev->entity_id, tile->entity_id) != 0) {
            continue;
        }
        tile->has_state = prev->has_state;
        tile->dimmable = prev->dimmable;
        tile->brightness_pct = prev->brightness_pct;
        memcpy(tile->state, prev->state, sizeof(tile->state));
        memcpy(tile->unit, prev->unit, sizeof(tile->unit));
        memcpy(tile->device_class, prev->device_class, sizeof(tile->device_class));
        if (!tile->name_from_card) {
            memcpy(tile->name, prev->name, sizeof(tile->name));
        }
        if (!tile->icon_from_card) {
            memcpy(tile->icon, prev->icon, sizeof(tile->icon));
        }
        return;
    }
}

static void show_error(const char *message)
{
    ESP_LOGW(TAG, "%s", message);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap->tile_count) {
        s_snap->tile_count = 0;
        s_snap->title[0] = '\0';
        s_snap->layout_generation++;
    }
    s_snap->status = HA_DASH_STATUS_ERROR;
    strlcpy(s_snap->message, message, sizeof(s_snap->message));
    xSemaphoreGive(s_lock);
    notify_changed();
}

static const char *dashboard_label(void)
{
    return s_cfg.dashboard[0] ? s_cfg.dashboard : "default";
}

static void apply_config(const cJSON *result)
{
    ha_dash_snapshot_t *next = dash_alloc(sizeof(*next));
    if (next == NULL) {
        set_status(HA_DASH_STATUS_ERROR, "Out of memory");
        return;
    }
    int count = ha_dash_layout_parse(result, next);
    if (count < 0) {
        free(next);
        char message[HA_DASH_MESSAGE_LEN];
        if (cJSON_GetObjectItemCaseSensitive(result, "strategy")) {
            snprintf(message, sizeof(message), "\"%s\" is auto-generated; take control of it in HA",
                     dashboard_label());
        } else {
            snprintf(message, sizeof(message), "\"%s\" has no views", dashboard_label());
        }
        show_error(message);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < next->tile_count; i++) {
        carry_over_state(&next->tiles[i], s_snap);
    }
    memcpy(s_snap->title, next->title, sizeof(s_snap->title));
    memcpy(s_snap->tiles, next->tiles, sizeof(s_snap->tiles));
    s_snap->tile_count = next->tile_count;
    s_snap->layout_generation++;
    s_snap->status = HA_DASH_STATUS_READY;
    s_snap->message[0] = '\0';
    xSemaphoreGive(s_lock);
    free(next);

    ESP_LOGI(TAG, "dashboard '%s': %d tiles (internal RAM free %u, largest block %u)",
             dashboard_label(), count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    notify_changed();
    subscribe_entities();
}

static void handle_result(const cJSON *msg, int id)
{
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(msg, "success");
    if (cJSON_IsTrue(success)) {
        if (id == s_config_request_id) {
            apply_config(cJSON_GetObjectItemCaseSensitive(msg, "result"));
        } else if (id == s_boards_request_id) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snap->board_count = (uint8_t)ha_dash_parse_dashboards(
                cJSON_GetObjectItemCaseSensitive(msg, "result"), s_snap->boards, HA_DASH_MAX_BOARDS);
            s_snap->boards_revision++;
            xSemaphoreGive(s_lock);
            notify_changed();
        }
        return;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(error, "message");
    ESP_LOGW(TAG, "request %d failed: %s", id, cJSON_IsString(text) ? text->valuestring : "?");

    if (id == s_config_request_id) {
        char message[HA_DASH_MESSAGE_LEN];
        if (cJSON_IsString(code) && strcmp(code->valuestring, "config_not_found") == 0) {
            /* Also returned for dashboards that were never edited in HA. */
            snprintf(message, sizeof(message), "\"%s\" has no saved cards", dashboard_label());
        } else {
            snprintf(message, sizeof(message), "%s",
                     cJSON_IsString(text) ? text->valuestring : "Could not load the dashboard");
        }
        show_error(message);
    }
}

static void handle_event(const cJSON *msg, int id)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");

    if (id == s_entities_sub_id && s_entities_sub_id != 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool changed = ha_dash_state_apply(s_snap, event);
        xSemaphoreGive(s_lock);
        if (changed) {
            notify_changed();
        }
    } else if (id == s_lovelace_sub_id && s_lovelace_sub_id != 0) {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(event, "data"), "url_path");
        bool ours = cJSON_IsString(path) ? strcmp(path->valuestring, s_cfg.dashboard) == 0
                                         : s_cfg.dashboard[0] == '\0';
        if (ours) {
            ESP_LOGI(TAG, "dashboard edited in Home Assistant, reloading");
            request_config();
        }
    }
}

static void handle_object(const cJSON *msg)
{
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(msg, "type");
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    int id = cJSON_IsNumber(id_item) ? id_item->valueint : 0;

    if (strcmp(type, "event") == 0) {
        handle_event(msg, id);
    } else if (strcmp(type, "result") == 0) {
        handle_result(msg, id);
    } else if (strcmp(type, "auth_required") == 0) {
        send_auth();
    } else if (strcmp(type, "auth_ok") == 0) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(msg, "ha_version");
        ESP_LOGI(TAG, "authenticated with Home Assistant %s",
                 cJSON_IsString(version) ? version->valuestring : "");
        s_authed = true;
        s_auth_failures = 0;
        set_status(HA_DASH_STATUS_LOADING, "Loading dashboard");
        subscribe_lovelace_updates();
        request_dashboards();
        request_config();
    } else if (strcmp(type, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "access token rejected");
        if (s_cfg.refresh_token[0] && s_auth_failures++ == 0) {
            /* Possibly just expired: mint a new one on the automatic reconnect. */
            s_access_token[0] = '\0';
            set_status(HA_DASH_STATUS_CONNECTING, "Signing in again");
            return;
        }
        client_stop(); /* retrying with the same token would only fail again */
        set_status(HA_DASH_STATUS_AUTH_FAILED,
                   s_cfg.refresh_token[0] ? "Signed out; sign in again" : "Access token rejected");
    }
}

static void handle_message(char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGW(TAG, "could not parse a %u-byte message (out of memory or invalid JSON)",
                 (unsigned)strlen(text));
        return;
    }
    if (cJSON_IsArray(root)) { /* coalesced messages */
        const cJSON *msg;
        cJSON_ArrayForEach(msg, root) {
            handle_object(msg);
        }
    } else {
        handle_object(root);
    }
    cJSON_Delete(root);
}

static void select_dashboard(int index)
{
    char path[HA_DASH_PATH_LEN];
    bool valid = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= 0 && index < s_snap->board_count) {
        strlcpy(path, s_snap->boards[index].url_path, sizeof(path));
        strlcpy(s_snap->dashboard, path, sizeof(s_snap->dashboard));
        valid = true;
    }
    xSemaphoreGive(s_lock);
    if (!valid) {
        return;
    }

    strlcpy(s_cfg.dashboard, path, sizeof(s_cfg.dashboard));
    if (ha_dash_config_set(&s_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the dashboard choice");
    }
    ESP_LOGI(TAG, "switching to dashboard '%s'", dashboard_label());
    set_status(HA_DASH_STATUS_LOADING, "Loading dashboard");
    request_config();
}

static void handle_action(const struct view_data_ha_dash_action *action)
{
    if (action->action == HA_DASH_ACTION_LIST_DASHBOARDS ||
        action->action == HA_DASH_ACTION_SELECT_DASHBOARD) {
        if (!s_authed) {
            ESP_LOGW(TAG, "ignoring dashboard action: not connected");
        } else if (action->action == HA_DASH_ACTION_LIST_DASHBOARDS) {
            request_dashboards();
        } else {
            select_dashboard(action->value);
        }
        return;
    }

    ha_dash_tile_t tile;
    bool valid = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (action->layout_generation == s_snap->layout_generation &&
        action->index < s_snap->tile_count) {
        tile = s_snap->tiles[action->index];
        valid = true;
    }
    xSemaphoreGive(s_lock);

    if (!valid || !s_authed) {
        ESP_LOGW(TAG, "ignoring action for tile %u (%s)", action->index,
                 valid ? "not connected" : "stale layout");
        return;
    }

    char msg[DASH_ACTION_BUF];
    int n = ha_dash_build_action(&tile, (ha_dash_action_t)action->action, action->value,
                                 ++s_next_id, msg, sizeof(msg));
    if (n > 0) {
        ESP_LOGI(TAG, "action on %s", tile.entity_id);
        send_text(msg);
    }
}

static void client_start(void);

static void login_result(bool ok, const char *message)
{
    if (ok) {
        ESP_LOGI(TAG, "sign-in: %s", message);
    } else {
        ESP_LOGW(TAG, "sign-in failed: %s", message);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap->login_ok = ok;
    strlcpy(s_snap->login_message, message, sizeof(s_snap->login_message));
    s_snap->login_revision++;
    xSemaphoreGive(s_lock);
    notify_changed();
}

static void handle_login(dash_login_t *login)
{
    ha_dash_config_t cfg;
    ha_dash_config_get(&cfg);
    if (login->address[0] && !ha_dash_websocket_url(login->address, cfg.url, sizeof(cfg.url))) {
        login_result(false, "That address doesn't look right");
        return;
    }
    if (cfg.url[0] == '\0') {
        login_result(false, "Enter the Home Assistant address");
        return;
    }
    if (!s_network) {
        login_result(false, "No Wi-Fi connection");
        return;
    }

    char refresh[sizeof(cfg.refresh_token)];
    char error[HA_DASH_MESSAGE_LEN];
    ESP_LOGI(TAG, "signing in to %s as %s", cfg.url, login->username);
    esp_err_t err = ha_dash_auth_login(cfg.url, login->username, login->password,
                                       refresh, sizeof(refresh), error, sizeof(error));
    if (err != ESP_OK) {
        login_result(false, error);
        return;
    }

    strlcpy(cfg.refresh_token, refresh, sizeof(cfg.refresh_token));
    strlcpy(cfg.username, login->username, sizeof(cfg.username));
    cfg.token[0] = '\0'; /* the login replaces any long-lived token */
    memset(refresh, 0, sizeof(refresh));
    if (ha_dash_config_set(&cfg) != ESP_OK) {
        login_result(false, "Signed in, but saving failed");
        return;
    }
    s_access_token[0] = '\0';
    s_auth_failures = 0;
    login_result(true, "Signed in");
    client_start();
}

static void client_stop(void)
{
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    rx_reset(); /* the WebSocket task is gone, so this is safe here */
    s_authed = false;
    s_entities_sub_id = 0;
    s_lovelace_sub_id = 0;
    s_config_request_id = 0;
    s_boards_request_id = 0;
}

static void client_start(void)
{
    client_stop();

    esp_err_t configured = ha_dash_config_get(&s_cfg);
    snapshot_connection();
    if (configured != ESP_OK) {
        set_status(HA_DASH_STATUS_UNCONFIGURED, "Sign in on the settings page");
        return;
    }
    if (!s_network) {
        set_status(HA_DASH_STATUS_WAITING_NETWORK, "Waiting for Wi-Fi");
        return;
    }

    esp_websocket_client_config_t config = {
        .uri = s_cfg.url,
        .buffer_size = 4096,
        .task_stack = 6144,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
        .crt_bundle_attach = esp_crt_bundle_attach, /* only used for wss:// */
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) {
        set_status(HA_DASH_STATUS_ERROR, "Invalid Home Assistant URL");
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        client_stop();
        set_status(HA_DASH_STATUS_ERROR, "Could not start the connection");
        return;
    }
    ESP_LOGI(TAG, "connecting to %s (dashboard '%s')", s_cfg.url, s_cfg.dashboard);
    set_status(HA_DASH_STATUS_CONNECTING, "Connecting to Home Assistant");
}

static void dash_task(void *arg)
{
    dash_item_t item;
    for (;;) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (item.kind) {
            case DASH_ITEM_NETWORK_UP:
                s_network = true;
                if (s_client == NULL) {
                    client_start();
                }
                break;
            case DASH_ITEM_NETWORK_DOWN:
                s_network = false;
                client_stop();
                set_status(HA_DASH_STATUS_WAITING_NETWORK, "Waiting for Wi-Fi");
                break;
            case DASH_ITEM_RELOAD:
                client_start();
                break;
            case DASH_ITEM_CONNECTED:
                s_authed = false;
                s_next_id = 0;
                s_entities_sub_id = 0;
                s_lovelace_sub_id = 0;
                set_status(HA_DASH_STATUS_CONNECTING, "Signing in");
                break;
            case DASH_ITEM_DISCONNECTED:
                if (s_client != NULL) {
                    s_authed = false;
                    set_status(HA_DASH_STATUS_CONNECTING, "Reconnecting");
                }
                break;
            case DASH_ITEM_MESSAGE:
                handle_message(item.text);
                free(item.text);
                break;
            case DASH_ITEM_ACTION:
                handle_action(&item.action);
                break;
            case DASH_ITEM_LOGIN:
                handle_login(item.login);
                memset(item.login, 0, sizeof(*item.login)); /* drop the password */
                free(item.login);
                break;
        }
    }
}

/* ── View events ────────────────────────────────────────────────────────── */

static void view_event_handler(void *context, enum view_event_id id, const void *payload)
{
    dash_item_t item = {0};
    if (id == VIEW_EVENT_WIFI_ST) {
        const struct view_data_wifi_st *wifi = payload;
        item.kind = wifi->is_network ? DASH_ITEM_NETWORK_UP : DASH_ITEM_NETWORK_DOWN;
        post_item(&item, pdMS_TO_TICKS(100));
    } else if (id == VIEW_EVENT_HA_DASH_ACTION) {
        item.kind = DASH_ITEM_ACTION;
        item.action = *(const struct view_data_ha_dash_action *)payload;
        post_item(&item, 0);
    }
}

void ha_dash_reload(void)
{
    dash_item_t item = {.kind = DASH_ITEM_RELOAD};
    post_item(&item, pdMS_TO_TICKS(100));
}

void ha_dash_login(const char *address, const char *username, const char *password)
{
    dash_item_t item = {.kind = DASH_ITEM_LOGIN, .login = dash_alloc(sizeof(dash_login_t))};
    if (item.login == NULL) {
        return;
    }
    strlcpy(item.login->address, address ? address : "", sizeof(item.login->address));
    strlcpy(item.login->username, username ? username : "", sizeof(item.login->username));
    strlcpy(item.login->password, password ? password : "", sizeof(item.login->password));
    if (!post_item(&item, pdMS_TO_TICKS(100))) {
        memset(item.login, 0, sizeof(*item.login));
        free(item.login);
    }
}

bool ha_dash_snapshot(ha_dash_snapshot_t *out)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_snap, sizeof(*out));
    xSemaphoreGive(s_lock);
    return true;
}

int ha_dash_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    s_snap = dash_alloc(sizeof(*s_snap));
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(DASH_QUEUE_LEN, sizeof(dash_item_t));
    if (s_snap == NULL || s_lock == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "init failed: out of memory");
        return ESP_ERR_NO_MEM;
    }

    s_snap->status = ha_dash_config_get(&s_cfg) == ESP_OK ? HA_DASH_STATUS_WAITING_NETWORK
                                                          : HA_DASH_STATUS_UNCONFIGURED;
    snapshot_connection();
    strlcpy(s_snap->message,
            s_snap->status == HA_DASH_STATUS_UNCONFIGURED ? "Sign in on the settings page"
                                                          : "Waiting for Wi-Fi",
            sizeof(s_snap->message));

    if (xTaskCreate(dash_task, "ha_dash", DASH_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start task");
        return ESP_FAIL;
    }
    if (!view_event_subscribe(VIEW_EVENT_WIFI_ST, view_event_handler, NULL) ||
        !view_event_subscribe(VIEW_EVENT_HA_DASH_ACTION, view_event_handler, NULL)) {
        ESP_LOGE(TAG, "failed to subscribe to view events");
        return ESP_FAIL;
    }
    notify_changed();
    return ESP_OK;
}
