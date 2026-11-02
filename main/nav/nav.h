#ifndef NAV_H
#define NAV_H

#include "lvgl.h"

/* Tile indices for the main swipeable screens */
#define NAV_TILE_HA_DASH  0   /* Home Assistant dashboard mirror */
#define NAV_TILE_COUNT    1

int      nav_init(void);
lv_obj_t *nav_get_tile(int tile_idx);   /* returns the container for that tile */
void     nav_go_tile(int tile_idx);     /* programmatic navigation */

#endif /* NAV_H */
