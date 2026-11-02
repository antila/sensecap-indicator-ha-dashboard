/**
 * PC simulator entry point — macOS SDL2.
 *
 * Architecture:
 *  - Single thread: lv_timer_handler() and SDL event loop run here.
 *  - lv_port_sem_take/give are no-ops (see lv_port_sim.c).
 *  - esp_event_post_to dispatches synchronously (see stubs/esp_event_stub.c).
 *  - Mock data layer fires timer-based events to drive the UI (mock/).
 *
 * Step 4 (SDL2 main loop) will fill in the display/input init.
 * Step 5 (mock data) will call mock_sensors_start() / mock_ha_start().
 */
#include "lvgl.h"
#include "lv_port_sim.h"
#include "view_data.h"

/* View init functions — called directly instead of via indicator_view.c */
#include "nav/nav.h"
#include "display/display_view.h"
#include "settings/settings_view.h"
#include "wifi/wifi_view.h"
#include "ha/ha_dash.h"
#include "ha/ha_dash_screen.h"

#include "mock/mock_sensors.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("[sim] sensecap-indicator simulator starting\n");

    /* 1. Init LVGL + SDL2 display + input */
    lv_init();
    lv_port_sim_init();   /* creates SDL2 window 480×800, wires mouse input */

    /* 2. Init view layer (hardware-free UI domains) */
    nav_init();
    indicator_display_view_init();
    indicator_wifi_view_init();
    ha_dash_screen_create();
    settings_view_init();
    ha_dash_init();       /* stub: shows a fixed demo dashboard */

    const char *open_settings = getenv("SIM_OPEN_SETTINGS");
    if (open_settings && *open_settings) {
        settings_view_show();
    }

    /* 3. Start mock data injection (Step 5) */
    mock_sensors_start();

    printf("[sim] entering main loop\n");

    /* 4. Main loop — single thread, no FreeRTOS */
    lv_port_sim_run();   /* blocks; returns on window close */

    lv_deinit();
    printf("[sim] exiting\n");
    return 0;
}
