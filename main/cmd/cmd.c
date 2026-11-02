#include "cmd.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"

#include "home_assistant_config.h"
#include "ha.h"
#include "ha_tls.h"
#include "ha_dash.h"
#include "ha_dash_config.h"
#include "ha_dash_layout.h"
#include "storage_nvs.h"
#include "indicator_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

static const char *TAG = "CMD_RESP";

#define PROMPT_STR "Indicator"

ESP_EVENT_DEFINE_BASE(CMD_CFG_EVENT_BASE);
esp_event_loop_handle_t cmd_cfg_event_handle;

static ha_cfg_interface ha_cfg;

static void print_mqtt_usage(void) {
    printf("\nMQTT configuration\n");
    printf("  Show current config:\n");
    printf("    haconfig\n\n");
    printf("  Set broker, client id and credentials:\n");
    printf("    setmqtt -a 192.168.1.10 -c indicator-01 -u mqtt_user -p mqtt_password\n");
    printf("    setmqtt --addr mqtt://192.168.1.10:1883\n\n");
    printf("  TLS (mqtts://):\n");
    printf("    setmqtt --addr mqtts://192.168.1.10:8883      (port defaults to 8883)\n");
    printf("    setmqttca            paste your CA certificate PEM, then it is stored\n");
    printf("    setmqttca -c         clear the stored CA (fall back to public CA bundle)\n");
    printf("    setmqtt --insecure   encrypt but skip server verification (discouraged)\n\n");
    printf("  Notes:\n");
    printf("    - The screen MQTT page only asks for the broker IP. It builds mqtt://<ip>:1883.\n");
    printf("    - TLS setup is console-only; a private-CA broker needs 'setmqttca' first.\n");
    printf("    - Restart is automatic after setmqtt/setmqttca succeeds.\n\n");
    printf("MQTT topics and payloads\n");
    printf("  Sensor data from device:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SENSOR_DATA);
    printf("    data : {\"temp\":\"23.5\"}, {\"humidity\":\"55\"}, {\"co2\":\"600\"}, {\"tvoc\":\"12\"}\n\n");
    printf("  Control device from Home Assistant/MQTT client:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SWITCH_SET);
    printf("    data : {\"switch1\":1}, {\"switch1\":0}, {\"switch5\":24}, {\"switch8\":50}\n\n");
    printf("  Device publishes control state:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SWITCH_STATE);
    printf("    data : {\"switch1\":1}, {\"switch5\":24}, {\"switch8\":50}\n\n");
}

static bool normalize_broker_url(const char *input, char *output, size_t output_size) {
    if (!input || input[0] == '\0' || !output || output_size == 0) return false;

    int written = 0;
    if (strncmp(input, "mqtt://", 7) == 0 || strncmp(input, "mqtts://", 8) == 0)
        written = snprintf(output, output_size, "%s", input);
    else
        written = snprintf(output, output_size, "mqtt://%s", input);

    return written > 0 && (size_t)written < output_size;
}

static int read_ha_config(int argc, char **argv) {
    ha_cfg_get(&ha_cfg);
    ESP_LOGI(TAG, "| Broker Address               | %-40s |", ha_cfg.broker_url);
    ESP_LOGI(TAG, "| Client ID                    | %-40s |", ha_cfg.client_id);
    ESP_LOGI(TAG, "| MQTT username                | %-40s |", ha_cfg.username);
    ESP_LOGI(TAG, "| MQTT password                | %-40s |", ha_cfg.password[0] ? "****" : "(not set)");
    if (ha_tls_url_is_tls(ha_cfg.broker_url)) {
        size_t ca_len = 0;
        char *ca = ha_tls_ca_load(&ca_len);
        ESP_LOGI(TAG, "| TLS mode                     | %-40s |",
                 ha_tls_mode_get() == HA_TLS_MODE_INSECURE ? "INSECURE (no verification)" : "verify");
        if (ca != NULL) {
            char ca_status[41];
            snprintf(ca_status, sizeof(ca_status), "stored (%u bytes)", (unsigned)ca_len);
            ESP_LOGI(TAG, "| TLS custom CA                | %-40s |", ca_status);
            free(ca);
        } else {
            ESP_LOGI(TAG, "| TLS custom CA                | %-40s |", "(none — public CA bundle)");
        }
    } else {
        ESP_LOGI(TAG, "| TLS                          | %-40s |", "off (mqtt:// broker URL)");
    }
    ESP_LOGI(TAG, "Run 'mqtthelp' for setmqtt examples and MQTT topic/payload examples.");
    return 0;
}

static int mqtt_help(int argc, char **argv) {
    print_mqtt_usage();
    return 0;
}

static void register_read_config(void) {
    const esp_console_cmd_t cmd = {
        .command = "haconfig",
        .help    = "Show current MQTT broker/client configuration",
        .hint    = NULL,
        .func    = &read_ha_config,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_mqtt_help(void) {
    const esp_console_cmd_t cmd = {
        .command = "mqtthelp",
        .help    = "Show MQTT setup examples, topics and payloads",
        .hint    = NULL,
        .func    = &mqtt_help,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

struct {
    struct arg_str *username;
    struct arg_str *password;
    struct arg_str *broker_url;
    struct arg_str *client_id;
    struct arg_lit *insecure;
    struct arg_lit *secure;
    struct arg_end *end;
} mqtt_args;

static int mqtt_config_set(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&mqtt_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, mqtt_args.end, argv[0]);
        print_mqtt_usage();
        return 1;
    }

    if (!mqtt_args.username->count && !mqtt_args.password->count &&
        !mqtt_args.broker_url->count && !mqtt_args.client_id->count &&
        !mqtt_args.insecure->count && !mqtt_args.secure->count) {
        print_mqtt_usage();
        return 0;
    }

    if (mqtt_args.insecure->count > 0 && mqtt_args.secure->count > 0) {
        ESP_LOGE(TAG, "--insecure and --secure are mutually exclusive");
        return 1;
    }
    if (mqtt_args.insecure->count > 0) {
        if (ha_tls_mode_set(HA_TLS_MODE_INSECURE) != ESP_OK) return 1;
        ESP_LOGW(TAG, "TLS verification DISABLED — the broker's certificate will not be checked.");
    }
    if (mqtt_args.secure->count > 0) {
        if (ha_tls_mode_set(HA_TLS_MODE_VERIFY) != ESP_OK) return 1;
        ESP_LOGI(TAG, "TLS verification enabled (stored CA if set, else public CA bundle).");
    }

    ha_cfg_get(&ha_cfg);

    if (mqtt_args.username->count > 0) {
        memset(ha_cfg.username, 0, sizeof(ha_cfg.username));
        strncpy(ha_cfg.username, mqtt_args.username->sval[0], sizeof(ha_cfg.username) - 1);
        ESP_LOGI(TAG, "Set MQTT username: %s", ha_cfg.username);
    }
    if (mqtt_args.password->count > 0) {
        memset(ha_cfg.password, 0, sizeof(ha_cfg.password));
        strncpy(ha_cfg.password, mqtt_args.password->sval[0], sizeof(ha_cfg.password) - 1);
        ESP_LOGI(TAG, "Set MQTT password: %s", ha_cfg.password[0] ? "****" : "(not set)");
    }
    if (mqtt_args.broker_url->count > 0) {
        char broker_url[sizeof(ha_cfg.broker_url)];
        if (!normalize_broker_url(mqtt_args.broker_url->sval[0], broker_url, sizeof(broker_url))) {
            ESP_LOGE(TAG, "Invalid or too long broker URL: %s", mqtt_args.broker_url->sval[0]);
            return 1;
        }
        memset(ha_cfg.broker_url, 0, sizeof(ha_cfg.broker_url));
        strncpy(ha_cfg.broker_url, broker_url, sizeof(ha_cfg.broker_url) - 1);
        ESP_LOGI(TAG, "Set MQTT broker URL: %s", ha_cfg.broker_url);
    }
    if (mqtt_args.client_id->count > 0) {
        memset(ha_cfg.client_id, 0, sizeof(ha_cfg.client_id));
        strncpy(ha_cfg.client_id, mqtt_args.client_id->sval[0], sizeof(ha_cfg.client_id) - 1);
        ESP_LOGI(TAG, "Set MQTT client ID: %s", ha_cfg.client_id);
    }

    if (ha_cfg_set(&ha_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT configuration");
        return 1;
    }
    esp_event_post_to(ha_cfg_event_handle, HA_CFG_EVENT_BASE, HA_CFG_SET, NULL, 0, portMAX_DELAY);
    ESP_LOGI(TAG, "MQTT configuration saved. Reconnecting MQTT client.");
    return 0;
}

static void register_mqtt_config(void) {
    mqtt_args.username   = arg_str0("u", "usr", "<username>", "MQTT username");
    mqtt_args.password   = arg_str0("p", "psw", "<password>", "MQTT password");
    mqtt_args.broker_url = arg_str0("a", "addr", "<broker_url>",
                                    "MQTT broker URL, e.g. 192.168.1.10, mqtt://host:1883 or mqtts://host");
    mqtt_args.client_id  = arg_str0("c", "id", "<client_id>", "MQTT client ID");
    mqtt_args.insecure   = arg_lit0(NULL, "insecure", "mqtts: skip server certificate verification");
    mqtt_args.secure     = arg_lit0(NULL, "secure", "mqtts: verify server certificate (default)");
    mqtt_args.end        = arg_end(6);

    const esp_console_cmd_t cmd = {
        .command  = "setmqtt",
        .help     = "Set MQTT config. Example: setmqtt -a 192.168.1.10 -c indicator-01 -u user -p pass",
        .hint     = NULL,
        .func     = &mqtt_config_set,
        .argtable = &mqtt_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

struct {
    struct arg_lit *clear;
    struct arg_end *end;
} mqttca_args;

/* Read a PEM certificate from the console until its END marker line.
 * Runs inside the REPL task while it executes this command, so reading stdin
 * here is race-free. select() bounds each line wait so a forgotten paste
 * cannot wedge the console forever. */
static int read_pem_from_console(char *buf, size_t buf_size) {
    size_t used = 0;
    printf("Paste the CA certificate PEM now (ends with -----END CERTIFICATE-----).\n");
    printf("Max %u bytes, 30 s per-line timeout, Ctrl-C style abort: just wait it out.\n",
           (unsigned)(buf_size - 1));

    while (used < buf_size - 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fileno(stdin), &rfds);
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        int sel = select(fileno(stdin) + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            printf("setmqttca: timed out waiting for certificate data\n");
            return -1;
        }
        char line[128];
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("setmqttca: read error\n");
            return -1;
        }
        size_t line_len = strlen(line);
        if (used + line_len >= buf_size - 1) {
            printf("setmqttca: certificate exceeds %u bytes\n", (unsigned)(buf_size - 1));
            return -1;
        }
        memcpy(buf + used, line, line_len);
        used += line_len;
        buf[used] = '\0';
        if (strstr(line, "-----END CERTIFICATE-----") != NULL) {
            return (int)used;
        }
    }
    printf("setmqttca: certificate exceeds %u bytes\n", (unsigned)(buf_size - 1));
    return -1;
}

static int mqtt_ca_set(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&mqttca_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, mqttca_args.end, argv[0]);
        return 1;
    }

    if (mqttca_args.clear->count > 0) {
        if (ha_tls_ca_clear() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear stored CA");
            return 1;
        }
        ESP_LOGI(TAG, "Stored CA cleared — mqtts:// now verifies against the public CA bundle.");
        esp_event_post_to(ha_cfg_event_handle, HA_CFG_EVENT_BASE, HA_CFG_SET, NULL, 0, portMAX_DELAY);
        return 0;
    }

    char *pem = malloc(HA_TLS_CA_MAX_LEN + 1);
    if (pem == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        return 1;
    }
    int len = read_pem_from_console(pem, HA_TLS_CA_MAX_LEN + 1);
    if (len <= 0) {
        free(pem);
        return 1;
    }
    if (ha_tls_ca_store(pem, (size_t)len) != ESP_OK) {
        ESP_LOGE(TAG, "Certificate rejected (must be a PEM CERTIFICATE block, %u bytes max)",
                 (unsigned)HA_TLS_CA_MAX_LEN);
        free(pem);
        return 1;
    }
    free(pem);
    ESP_LOGI(TAG, "CA stored (%d bytes). mqtts:// connections now verify against it.", len);
    ESP_LOGI(TAG, "Reconnecting MQTT client.");
    esp_event_post_to(ha_cfg_event_handle, HA_CFG_EVENT_BASE, HA_CFG_SET, NULL, 0, portMAX_DELAY);
    return 0;
}

static void register_mqtt_ca(void) {
    mqttca_args.clear = arg_lit0("c", "clear", "clear the stored CA certificate");
    mqttca_args.end   = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command  = "setmqttca",
        .help     = "Store a CA certificate for mqtts:// (paste PEM), or clear it with -c",
        .hint     = NULL,
        .func     = &mqtt_ca_set,
        .argtable = &mqttca_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void print_dash_usage(void) {
    printf("\nHome Assistant dashboard\n");
    printf("  The first screen mirrors a Lovelace dashboard over the WebSocket API.\n\n");
    printf("  Sign in with your Home Assistant account (only a refresh token is stored):\n");
    printf("    setdash -a 192.168.1.10 -u <username> -p <password>\n");
    printf("  ...or use a long-lived access token:\n");
    printf("    setdash -a 192.168.1.10 -t <long-lived-token>\n");
    printf("  Pick the dashboard (also possible on the settings page):\n");
    printf("    setdash -d touch-screen\n");
    printf("    setdash -a https://example.ui.nabu.casa\n");
    printf("    setdash -d default        (Home Assistant's default dashboard)\n\n");
    printf("  Show current settings:\n");
    printf("    dashconfig\n\n");
    printf("  Notes:\n");
    printf("    - A bare host uses port 8123. The dashboard is its URL path, e.g.\n");
    printf("      'touch-screen' for http://ha:8123/touch-screen.\n");
    printf("    - Tokens are stored unencrypted on the device; a dedicated user is safer.\n");
    printf("    - Two-factor login is not supported; use a long-lived token instead.\n\n");
}

static int dash_config_show(int argc, char **argv) {
    ha_dash_config_t cfg;
    ha_dash_config_get(&cfg);
    char token[48];
    if (cfg.refresh_token[0]) {
        snprintf(token, sizeof(token), "signed in as %s", cfg.username);
    } else if (cfg.token[0]) {
        snprintf(token, sizeof(token), "long-lived token (%u chars)", (unsigned)strlen(cfg.token));
    } else {
        snprintf(token, sizeof(token), "(not set)");
    }
    ESP_LOGI(TAG, "| Home Assistant URL           | %-40s |", cfg.url[0] ? cfg.url : "(not set)");
    ESP_LOGI(TAG, "| Login                        | %-40s |", token);
    ESP_LOGI(TAG, "| Dashboard                    | %-40s |", cfg.dashboard[0] ? cfg.dashboard : "(default)");
    return 0;
}

static void register_dash_show(void) {
    const esp_console_cmd_t cmd = {
        .command = "dashconfig",
        .help    = "Show the Home Assistant dashboard connection settings",
        .hint    = NULL,
        .func    = &dash_config_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

struct {
    struct arg_str *addr;
    struct arg_str *token;
    struct arg_str *username;
    struct arg_str *password;
    struct arg_str *dashboard;
    struct arg_end *end;
} dash_args;

static int dash_config_set(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&dash_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, dash_args.end, argv[0]);
        print_dash_usage();
        return 1;
    }
    if (!dash_args.addr->count && !dash_args.token->count && !dash_args.dashboard->count &&
        !dash_args.username->count && !dash_args.password->count) {
        print_dash_usage();
        return dash_config_show(0, NULL);
    }

    if (dash_args.username->count || dash_args.password->count) {
        if (!dash_args.username->count || !dash_args.password->count) {
            ESP_LOGE(TAG, "Signing in needs both -u <username> and -p <password>");
            return 1;
        }
        if (dash_args.token->count) {
            ESP_LOGE(TAG, "Use either -u/-p or -t, not both");
            return 1;
        }
        if (dash_args.dashboard->count) {
            ESP_LOGE(TAG, "Set the dashboard separately: setdash -d <url_path>");
            return 1;
        }
        ha_dash_login(dash_args.addr->count ? dash_args.addr->sval[0] : "",
                      dash_args.username->sval[0], dash_args.password->sval[0]);
        ESP_LOGI(TAG, "Signing in as %s; watch for 'ha-dash' log lines", dash_args.username->sval[0]);
        return 0;
    }

    ha_dash_config_t cfg;
    ha_dash_config_get(&cfg);

    if (dash_args.addr->count > 0) {
        if (!ha_dash_websocket_url(dash_args.addr->sval[0], cfg.url, sizeof(cfg.url))) {
            ESP_LOGE(TAG, "Invalid or too long address: %s", dash_args.addr->sval[0]);
            return 1;
        }
        ESP_LOGI(TAG, "Set Home Assistant URL: %s", cfg.url);
    }
    if (dash_args.token->count > 0) {
        if (strlen(dash_args.token->sval[0]) >= sizeof(cfg.token)) {
            ESP_LOGE(TAG, "Token is longer than %u characters", (unsigned)sizeof(cfg.token) - 1);
            return 1;
        }
        strlcpy(cfg.token, dash_args.token->sval[0], sizeof(cfg.token));
        cfg.refresh_token[0] = '\0'; /* the token replaces a username/password login */
        cfg.username[0] = '\0';
        ESP_LOGI(TAG, "Set access token: **** (%u chars)", (unsigned)strlen(cfg.token));
    }
    if (dash_args.dashboard->count > 0) {
        const char *path = dash_args.dashboard->sval[0];
        if (strcmp(path, "default") == 0 || strcmp(path, "lovelace") == 0) {
            path = "";
        }
        if (strlen(path) >= sizeof(cfg.dashboard)) {
            ESP_LOGE(TAG, "Dashboard path is too long");
            return 1;
        }
        strlcpy(cfg.dashboard, path, sizeof(cfg.dashboard));
        ESP_LOGI(TAG, "Set dashboard: %s", cfg.dashboard[0] ? cfg.dashboard : "(default)");
    }

    if (ha_dash_config_set(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save dashboard configuration");
        return 1;
    }
    if (!cfg.url[0] || (!cfg.token[0] && !cfg.refresh_token[0])) {
        ESP_LOGW(TAG, "Saved. Still needed: %s%s", cfg.url[0] ? "" : "-a <address> ",
                 (cfg.token[0] || cfg.refresh_token[0]) ? "" : "-u <user> -p <password> or -t <token>");
        return 0;
    }
    ha_dash_reload();
    ESP_LOGI(TAG, "Dashboard configuration saved. Reconnecting.");
    return 0;
}

static void register_dash_config(void) {
    dash_args.addr      = arg_str0("a", "addr", "<address>",
                                   "Home Assistant address, e.g. 192.168.1.10, ha.local:8123 or https://host");
    dash_args.token     = arg_str0("t", "token", "<token>", "Long-lived access token");
    dash_args.username  = arg_str0("u", "user", "<username>", "Home Assistant username (with -p)");
    dash_args.password  = arg_str0("p", "password", "<password>", "Home Assistant password, used once to sign in");
    dash_args.dashboard = arg_str0("d", "dashboard", "<url_path>",
                                   "Dashboard URL path, e.g. touch-screen; 'default' for the default dashboard");
    dash_args.end       = arg_end(6);

    const esp_console_cmd_t cmd = {
        .command  = "setdash",
        .help     = "Connect to Home Assistant. Example: setdash -a 192.168.1.10 -u <user> -p <password>",
        .hint     = NULL,
        .func     = &dash_config_set,
        .argtable = &dash_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

int indicator_cmd_init(void) {
    esp_console_repl_t       *repl        = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt                    = PROMPT_STR ">";
    repl_config.max_cmdline_length        = 1024;

    esp_event_loop_args_t cmd_event_task_args = {
        .queue_size      = 2,
        .task_name       = "cmd_event_task",
        .task_priority   = uxTaskPriorityGet(NULL),
        .task_stack_size = 4096,
        .task_core_id    = tskNO_AFFINITY,
    };
    ESP_ERROR_CHECK(esp_event_loop_create(&cmd_event_task_args, &cmd_cfg_event_handle));

    ha_cfg_get(&ha_cfg);
    register_read_config();
    register_mqtt_help();
    register_mqtt_config();
    register_mqtt_ca();
    register_dash_show();
    register_dash_config();

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return ESP_OK;
}
