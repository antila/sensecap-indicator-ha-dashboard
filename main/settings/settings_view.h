#ifndef SETTINGS_VIEW_H
#define SETTINGS_VIEW_H

#include "lvgl.h"

int settings_view_init(void);

/* Navigate to the settings page. */
void settings_view_show(void);

/* Empty container on the settings page where the Home Assistant view shows
 * its connection status. NULL before settings_view_init(). */
lv_obj_t *settings_view_ha_slot(void);

#endif /* SETTINGS_VIEW_H */
