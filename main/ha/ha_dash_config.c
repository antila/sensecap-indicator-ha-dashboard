#include "ha_dash_config.h"

#include <string.h>

#include "esp_log.h"
#include "storage_nvs.h"

static const char *TAG = "ha-dash-cfg";

#define DASH_URL_STORAGE   "dash-url"
#define DASH_TOKEN_STORAGE "dash-token"
#define DASH_BOARD_STORAGE "dash-board"

static void read_string(char *key, char *buf, size_t size)
{
    size_t len = size - 1;
    if (indicator_nvs_read(key, buf, &len) != ESP_OK) {
        len = 0;
    }
    buf[len < size ? len : size - 1] = '\0';
}

esp_err_t ha_dash_config_get(ha_dash_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    read_string(DASH_URL_STORAGE, cfg->url, sizeof(cfg->url));
    read_string(DASH_TOKEN_STORAGE, cfg->token, sizeof(cfg->token));

    /* Values are stored with their NUL terminator, so an explicitly stored
     * empty string (HA's default dashboard) is still a one-byte blob. */
    size_t len = sizeof(cfg->dashboard) - 1;
    if (indicator_nvs_read(DASH_BOARD_STORAGE, cfg->dashboard, &len) != ESP_OK) {
        strlcpy(cfg->dashboard, HA_DASH_DEFAULT_DASHBOARD, sizeof(cfg->dashboard));
    } else {
        cfg->dashboard[len] = '\0';
    }

    return (cfg->url[0] && cfg->token[0]) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ha_dash_config_set(const ha_dash_config_t *cfg)
{
    esp_err_t err = indicator_nvs_write(DASH_URL_STORAGE, (void *)cfg->url, strlen(cfg->url) + 1);
    if (err == ESP_OK) {
        err = indicator_nvs_write(DASH_TOKEN_STORAGE, (void *)cfg->token, strlen(cfg->token) + 1);
    }
    if (err == ESP_OK) {
        err = indicator_nvs_write(DASH_BOARD_STORAGE, (void *)cfg->dashboard, strlen(cfg->dashboard) + 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to store dashboard config: %s", esp_err_to_name(err));
    }
    return err;
}
