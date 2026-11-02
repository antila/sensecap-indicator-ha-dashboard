#ifndef HA_H
#define HA_H

/* Umbrella header — full HA domain access.
 * Prefer domain-specific headers when only one slice is needed:
 *   ha_config.h  — broker NVS config types and API
 *   ha_mqtt.h    — MQTT lifecycle, event loops, HA_CFG_EVENT
 *   ha_sensor.h  — sensor entity and MQTT routing
 *   ha_switch.h  — switch entity, MQTT routing, screen attachment
 *   ha_dash.h    — dashboard mirror over the Home Assistant WebSocket API
 */
#include "view_data.h"
#include "ha_config.h"
#include "ha_mqtt.h"
#include "ha_sensor.h"
#include "ha_switch.h"

#endif /* HA_H */
