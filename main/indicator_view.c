#include "indicator_enabler.h"

#include "nav.h"
#include "view_data.h"

extern int indicator_display_view_init(void);

int indicator_view_init(void) {
	nav_init();

#ifdef INDICATOR_DISPLAY_H
	indicator_display_view_init();
#endif

#ifdef WIFI_H
	indicator_wifi_view_init();
#endif

#ifdef SETTINGS_H
	settings_view_init();
#endif

#ifdef HA_H
	indicator_ha_view_init(); /* fills the settings page's status slot */
#endif

	return 0;
}
