/* Simulator stand-in for ha_dash.c: a fixed demo dashboard instead of a
 * Home Assistant WebSocket connection. Taps are logged, not applied. */
#include "ha_dash.h"
#include "view_event.h"

#include <stdio.h>
#include <string.h>

static ha_dash_tile_t *add(ha_dash_snapshot_t *s, ha_dash_kind_t kind, const char *entity,
                           const char *name, const char *state, const char *unit)
{
    ha_dash_tile_t *t = &s->tiles[s->tile_count++];
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->wide = kind == HA_DASH_KIND_HEADING;
    t->tap_enabled = true;
    t->brightness_pct = -1;
    t->has_state = kind != HA_DASH_KIND_HEADING;
    snprintf(t->entity_id, sizeof(t->entity_id), "%s", entity);
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->state, sizeof(t->state), "%s", state);
    snprintf(t->unit, sizeof(t->unit), "%s", unit);
    return t;
}

bool ha_dash_snapshot(ha_dash_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->status = HA_DASH_STATUS_READY;
    out->layout_generation = 1;
    snprintf(out->title, sizeof(out->title), "Simulator");
    add(out, HA_DASH_KIND_HEADING, "", "Lights", "", "");
    ha_dash_tile_t *t = add(out, HA_DASH_KIND_LIGHT, "light.ceiling", "Ceiling lights", "on", "");
    t->dimmable = true;
    t->brightness_pct = 72;
    add(out, HA_DASH_KIND_TOGGLE, "switch.coffee", "Coffee maker", "off", "");
    add(out, HA_DASH_KIND_HEADING, "", "Climate", "", "");
    t = add(out, HA_DASH_KIND_SENSOR, "sensor.outdoor", "Outdoor", "-3.4", "\xC2\xB0" "C");
    snprintf(t->device_class, sizeof(t->device_class), "temperature");
    t = add(out, HA_DASH_KIND_SENSOR, "sensor.humidity", "Humidity", "54", "%");
    snprintf(t->device_class, sizeof(t->device_class), "humidity");
    return true;
}

void ha_dash_reload(void) {}

int ha_dash_init(void)
{
    struct view_data_ha_dash_changed changed = {.layout_generation = 1};
    view_event_publish_state(VIEW_EVENT_HA_DASH_CHANGED, &changed, sizeof(changed));
    return 0;
}
