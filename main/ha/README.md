# HA Domain — Vertical Slice Architecture

Home Assistant integration. This domain is the pilot for the project's target architecture:
each feature slice owns its full stack (data model + MQTT protocol + UI component).

---

## Files and Responsibilities

| File | Lines | Owns |
|------|-------|------|
| `ha.h` | 50 | Public API for the entire domain. Include this, not individual slice headers. |
| `ha_mqtt.c` | ~200 | MQTT client lifecycle: connect/disconnect/reconnect. Routes `MQTT_EVENT_DATA` to sensor and switch slices. Owns `mqtt_ha_instance`. |
| `ha_config.c` | ~154 | Broker NVS config (`ha_cfg_get`/`ha_cfg_set`) + IP address modal view. |
| `ha_tls.c` | ~85 | `mqtts://` trust configuration: verification mode + custom CA stored under separate NVS keys. UI-free. |
| `ha_sensor.c` | ~128 | Sensor entity metadata, subscribes to sensor MQTT topics, publishes built-in sensor readings, routes incoming sensor JSON to `VIEW_EVENT_HA_SENSOR`. |
| `ha_switch.c` | Switch entity metadata, in-memory authoritative state, MQTT publish/subscribe, and generation-tagged publish results. UI-free. |
| `ha_switch_sync.c` | Pure generation/debounce/result state module. Owns stale-result filtering and rollback decisions. Currently has no on-screen user. |
| `ha_dash.c` | Dashboard mirror model: Home Assistant WebSocket client, auth, `lovelace/config`, `subscribe_entities`, `call_service`. Owns the snapshot behind a mutex. UI-free. |
| `ha_dash_config.c` | Dashboard URL, access token and dashboard path under their own NVS keys. |
| `ha_dash_layout.c` | Pure cJSON helpers: Lovelace config to tiles, entity state updates, service-call messages, URL normalization. Host-tested. |
| `ha_dash_present.c` | Pure icon/state-text helpers for tiles. Host-tested. |
| `ha_dash_screen.c` | Builds the dashboard page on `NAV_TILE_HA_DASH` from snapshots; posts tap/slider actions. |

---

## Init Sequence

```
indicator_ha_view_init()    (called from indicator_view.c)
  ha_config_view_init()     register VIEW_EVENT_MQTT_ADDR_CHANGED + VIEW_EVENT_HA_ADDR_DISPLAY
  ha_dash_screen_create()   builds the dashboard page on NAV_TILE_HA_DASH,
                            subscribes to VIEW_EVENT_HA_DASH_CHANGED

indicator_ha_model_init()   (called later from indicator_model.c)
  ha_sensor_init()          register VIEW_EVENT_SENSOR_DATA handler
  ha_switch_init()          init entities, publish zero startup values,
                            register VIEW_EVENT_HA_SWITCH_ST handler
  create ha_cfg_event_handle
  init mqtt_ha_instance
  register VIEW_EVENT_WIFI_ST handler
  ha_dash_init()            start the dashboard worker task, register
                            VIEW_EVENT_WIFI_ST + VIEW_EVENT_HA_DASH_ACTION
```

---

## Event Flows

```
MQTT broker sends switch command
  → ha_mqtt.c: MQTT_EVENT_DATA
  → ha_switch_on_mqtt_data()
  → update latest in-memory model value
  → publish keyed VIEW_EVENT_HA_SWITCH_SET {index, value} (no on-screen consumer)

WiFi connects
  → VIEW_EVENT_WIFI_ST {is_network=true}
  → ha_mqtt.c: post MQTT_APP_START to mqtt_app_event_handle
  → mqtt/mqtt.c: calls mqtt_ha_instance.mqtt_starter()
  → ha_mqtt.c: _mqtt_ha_start() reads NVS config, creates MQTT client

MQTT connects
  → MQTT_EVENT_CONNECTED
  → ha_sensor_subscribe() — subscribe to sensor topics
  → ha_switch_subscribe() — subscribe to switch topics, start restore task

Dashboard connects (WebSocket, independent of MQTT)
  → VIEW_EVENT_WIFI_ST {is_network=true} → ha_dash.c worker starts the client
  → auth with the long-lived token from `setdash`
  → lovelace/config for the configured dashboard → ha_dash_layout_parse()
  → subscribe_entities for the entities on it; subscribe to lovelace_updated
  → every snapshot change publishes VIEW_EVENT_HA_DASH_CHANGED (one coalesced slot)
  → ha_dash_screen.c copies the snapshot under the LVGL lock, rebuilds on a new
    layout_generation, otherwise updates only tiles whose revision changed

User taps a dashboard tile or releases a light slider
  → ha_dash_screen.c posts VIEW_EVENT_HA_DASH_ACTION {layout_generation, index, action, value}
  → ha_dash.c drops it if the layout changed meanwhile, else sends call_service
  → Home Assistant's state change comes back through subscribe_entities

User picks a dashboard (settings page → status card → picker)
  → opening the picker posts HA_DASH_ACTION_LIST_DASHBOARDS → lovelace/dashboards/list
  → selecting posts HA_DASH_ACTION_SELECT_DASHBOARD {value = index}
  → ha_dash.c saves the url_path with ha_dash_config_set() and refetches lovelace/config

Dashboard edited in Home Assistant
  → lovelace_updated event for our url_path → refetch config, carry live state over

User changes broker IP
  → ha_config.c: posts VIEW_EVENT_MQTT_ADDR_CHANGED
  → ha_config.c: validates, saves NVS, posts HA_CFG_BROKER_CHANGED
  → ha_mqtt.c: stop/set_uri/start MQTT client
```

---

## Known Issues

- The only screen is the dashboard mirror (`ha_dash_screen.c`). The MQTT sensor and switch slices still run, but `VIEW_EVENT_HA_SENSOR` and `VIEW_EVENT_HA_SWITCH_SET` have no on-screen consumer.

---

## Extending to a New Feature Slice

Follow this pattern:
1. `ha_yyy.c` — data + MQTT logic, registers event handlers in `ha_yyy_init()`
2. `ha_yyy_screen.c` — LVGL component, `create`/`update`/`destroy` interface
3. Declare cross-module functions in `ha.h`
4. Call `ha_yyy_init()` from `indicator_ha_model_init()` in `ha_mqtt.c`
5. Call `ha_yyy_screen_create()` from `indicator_ha_view_init()` in `ha_mqtt.c`
