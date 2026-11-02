#ifndef HA_DASH_SCREEN_H
#define HA_DASH_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build the Home Assistant dashboard page on NAV_TILE_HA_DASH and follow
 * VIEW_EVENT_HA_DASH_CHANGED. Call from the view init sequence. */
void ha_dash_screen_create(void);

#ifdef __cplusplus
}
#endif

#endif /* HA_DASH_SCREEN_H */
