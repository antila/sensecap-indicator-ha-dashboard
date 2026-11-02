#ifndef HA_DASH_PRESENT_H
#define HA_DASH_PRESENT_H

/* How a dashboard tile is shown: icon glyph, state text, and whether it
 * counts as "on". Pure helpers so the wording can be tested on the host. */

#include <stdbool.h>
#include <stddef.h>

#include "ha_dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "light.kitchen" -> "light". Returns false if there is no dot. */
bool ha_dash_entity_domain(const char *entity_id, char *domain, size_t len);

/* UTF-8 glyph from ui_font_dash_icons for the tile, based on its mdi icon,
 * then its domain and device class. Never returns NULL. */
const char *ha_dash_icon_glyph(const ha_dash_tile_t *tile);

/* True for states shown highlighted: on, open, unlocked, playing, ... */
bool ha_dash_tile_is_active(const ha_dash_tile_t *tile);

/* True when the entity is unavailable or has no state yet. */
bool ha_dash_tile_is_unavailable(const ha_dash_tile_t *tile);

/* Human-readable state: "On", "Off · 40%", "21.5", "Open", "Unavailable".
 * The unit is not included; sensors show tile->unit separately. */
void ha_dash_format_state(const ha_dash_tile_t *tile, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_PRESENT_H */
