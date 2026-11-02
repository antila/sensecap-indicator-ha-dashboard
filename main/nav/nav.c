#include "nav.h"

#include "lv_port.h"
#include "sdkconfig.h"

#define NAV_TILE_WIDTH  CONFIG_LCD_EVB_SCREEN_WIDTH
#define NAV_TILE_HEIGHT CONFIG_LCD_EVB_SCREEN_HEIGHT

static lv_obj_t *s_tileview;
static lv_obj_t *s_tiles[NAV_TILE_COUNT];

static void nav_style_base_obj(lv_obj_t *obj) {
	lv_obj_set_style_bg_color(obj, lv_color_hex(0x101418), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void nav_style_static_obj(lv_obj_t *obj) {
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	nav_style_base_obj(obj);
}

int nav_init(void) {
	lv_port_sem_take();

	lv_display_t *display = lv_display_get_default();
	lv_theme_t *theme = lv_theme_default_init(display,
											  lv_palette_main(LV_PALETTE_BLUE),
											  lv_palette_main(LV_PALETTE_RED),
											  true,
											  LV_FONT_DEFAULT);
	lv_display_set_theme(display, theme);

	lv_obj_t *screen = lv_screen_active();
	nav_style_static_obj(screen);

	s_tileview = lv_tileview_create(lv_screen_active());
	lv_obj_set_size(s_tileview, NAV_TILE_WIDTH, NAV_TILE_HEIGHT);
	lv_obj_set_pos(s_tileview, 0, 0);
	lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(s_tileview, LV_OBJ_FLAG_SCROLL_ELASTIC |
								  LV_OBJ_FLAG_SCROLL_MOMENTUM |
								  LV_OBJ_FLAG_SCROLL_CHAIN);
	nav_style_base_obj(s_tileview);

	for (int i = 0; i < NAV_TILE_COUNT; i++) {
		s_tiles[i] = lv_tileview_add_tile(s_tileview, i, 0,
										  LV_DIR_LEFT | LV_DIR_RIGHT);
		lv_obj_set_size(s_tiles[i], NAV_TILE_WIDTH, NAV_TILE_HEIGHT);
		nav_style_static_obj(s_tiles[i]);
	}
	lv_tileview_set_tile_by_index(s_tileview, NAV_TILE_HOME, 0, LV_ANIM_OFF);

	lv_port_sem_give();

	return 0;
}

lv_obj_t *nav_get_tile(int tile_idx) {
	if (tile_idx < 0 || tile_idx >= NAV_TILE_COUNT) {
		return NULL;
	}

	return s_tiles[tile_idx];
}

void nav_go_tile(int tile_idx) {
	if (tile_idx < 0 || tile_idx >= NAV_TILE_COUNT) {
		return;
	}

	lv_tileview_set_tile_by_index(s_tileview, tile_idx, 0, LV_ANIM_ON);
}
