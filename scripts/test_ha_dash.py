#!/usr/bin/env python3
"""Host-side tests for the Home Assistant dashboard parser and presentation.

Compiles main/ha/ha_dash_layout.c and ha_dash_present.c against ESP-IDF's
bundled cJSON. Needs IDF_PATH (or ~/esp/esp-idf-v5.5.6) for cJSON; the tests
are skipped when it cannot be found.
"""

import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HA = ROOT / "main" / "ha"


def find_cjson() -> Path | None:
    candidates = []
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]))
    candidates.extend(sorted(Path.home().glob("esp/esp-idf*"), reverse=True))
    for idf in candidates:
        cjson = idf / "components" / "json" / "cJSON"
        if (cjson / "cJSON.c").is_file():
            return cjson
    return None


CJSON = find_cjson()

PRELUDE = r"""
#include "ha_dash_layout.h"
#include "ha_dash_present.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ha_dash_snapshot_t snap;

__attribute__((unused)) static void parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    memset(&snap, 0, sizeof(snap));
    ha_dash_layout_parse(root, &snap);
    cJSON_Delete(root);
}

__attribute__((unused)) static bool apply(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    bool changed = ha_dash_state_apply(&snap, root);
    cJSON_Delete(root);
    return changed;
}

#define EXPECT_STR(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, (expected), (actual)); \
        return 1; \
    } \
} while (0)
"""


@unittest.skipIf(CJSON is None, "ESP-IDF cJSON not found; set IDF_PATH")
class HaDashTests(unittest.TestCase):
    def run_harness(self, body: str) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            harness = tmp / "harness.c"
            binary = tmp / "harness"
            harness.write_text(PRELUDE + textwrap.dedent(body), encoding="utf-8")
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(HA), "-I", str(CJSON),
                    str(HA / "ha_dash_layout.c"), str(HA / "ha_dash_present.c"),
                    str(CJSON / "cJSON.c"), str(harness),
                    "-lm", "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_sections_view_with_button_card(self):
        self.run_harness(r"""
        int main(void)
        {
            parse("{\"views\":[{\"title\":\"Touchscreen\",\"type\":\"sections\",\"sections\":[{\"type\":\"grid\",\"cards\":["
                  "{\"type\":\"button\",\"entity\":\"light.taklampor\",\"name\":\"Vardagsrum Taklampor\","
                  "\"icon\":\"mdi:ceiling-light-multiple\",\"tap_action\":{\"action\":\"toggle\"},"
                  "\"grid_options\":{\"columns\":6,\"rows\":3}}]}]}]}");
            assert(snap.tile_count == 1);
            EXPECT_STR(snap.title, "Touchscreen");
            ha_dash_tile_t *t = &snap.tiles[0];
            assert(t->kind == HA_DASH_KIND_LIGHT);
            assert(!t->wide && t->tap_enabled && t->name_from_card && t->icon_from_card);
            EXPECT_STR(t->name, "Vardagsrum Taklampor");
            EXPECT_STR(ha_dash_icon_glyph(t), "\xEF\x83\xAB"); /* lightbulb */
            EXPECT_STR(t->state, "");
            assert(t->brightness_pct == -1);
            return 0;
        }
        """)

    def test_nested_cards_entities_and_headings(self):
        self.run_harness(r"""
        int main(void)
        {
            parse("{\"views\":[{\"title\":\"Home\",\"cards\":["
                  "{\"type\":\"heading\",\"heading\":\"Kök\"},"
                  "{\"type\":\"vertical-stack\",\"cards\":["
                  "  {\"type\":\"tile\",\"entity\":\"switch.kaffe\",\"grid_options\":{\"columns\":\"full\"}},"
                  "  {\"type\":\"horizontal-stack\",\"cards\":[{\"type\":\"button\",\"entity\":\"scene.kvall\",\"tap_action\":{\"action\":\"none\"}}]}]},"
                  "{\"type\":\"entities\",\"title\":\"Klimat\",\"entities\":["
                  "  \"sensor.temp\", {\"entity\":\"binary_sensor.door\",\"name\":\"Ytterdörr\"},"
                  "  {\"type\":\"divider\"}, {\"type\":\"section\",\"label\":\"Mer\"}, \"switch.kaffe\"]},"
                  "{\"type\":\"markdown\",\"content\":\"hi\"},"
                  "{\"type\":\"conditional\",\"card\":{\"type\":\"entity\",\"entity\":\"lock.front\"}}]}]}");
            assert(snap.tile_count == 9);
            assert(snap.tiles[0].kind == HA_DASH_KIND_HEADING);
            EXPECT_STR(snap.tiles[0].name, "Kök");
            assert(snap.tiles[1].kind == HA_DASH_KIND_TOGGLE && snap.tiles[1].wide);
            EXPECT_STR(snap.tiles[1].name, "switch.kaffe");
            assert(snap.tiles[2].kind == HA_DASH_KIND_BUTTON && !snap.tiles[2].tap_enabled);
            EXPECT_STR(snap.tiles[3].name, "Klimat");
            assert(snap.tiles[4].kind == HA_DASH_KIND_SENSOR);
            EXPECT_STR(snap.tiles[5].name, "Ytterdörr");
            EXPECT_STR(snap.tiles[6].name, "Mer");
            EXPECT_STR(snap.tiles[7].entity_id, "switch.kaffe");
            EXPECT_STR(snap.tiles[8].entity_id, "lock.front");

            char ids[256];
            int n = ha_dash_layout_entity_ids_json(&snap, ids, sizeof(ids));
            assert(n > 0);
            EXPECT_STR(ids, "[\"switch.kaffe\",\"scene.kvall\",\"sensor.temp\",\"binary_sensor.door\",\"lock.front\"]");
            assert(ha_dash_layout_entity_ids_json(&snap, ids, 20) == -1);
            return 0;
        }
        """)

    def test_state_updates_and_actions(self):
        self.run_harness(r"""
        int main(void)
        {
            parse("{\"views\":[{\"cards\":[{\"type\":\"tile\",\"entity\":\"light.sofa\"},"
                  "{\"type\":\"tile\",\"entity\":\"sensor.temp\"},{\"type\":\"tile\",\"entity\":\"lock.front\"},"
                  "{\"type\":\"tile\",\"entity\":\"binary_sensor.door\"},{\"type\":\"tile\",\"entity\":\"scene.kvall\"}]}]}");
            ha_dash_tile_t *light = &snap.tiles[0], *temp = &snap.tiles[1], *lock = &snap.tiles[2], *door = &snap.tiles[3];

            assert(apply("{\"a\":{\"light.sofa\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"Soffa\",\"brightness\":128,"
                         "\"supported_color_modes\":[\"brightness\"]},\"c\":\"x\",\"lc\":1},"
                         "\"sensor.temp\":{\"s\":\"21.456\",\"a\":{\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"friendly_name\":\"Temp\"}},"
                         "\"lock.front\":{\"s\":\"locked\",\"a\":{}},"
                         "\"binary_sensor.door\":{\"s\":\"on\",\"a\":{\"device_class\":\"door\"}}}}"));
            EXPECT_STR(light->name, "Soffa");
            assert(light->dimmable && light->brightness_pct == 50 && light->revision == 1);
            assert(ha_dash_tile_is_active(light));
            char text[48];
            ha_dash_format_state(light, text, sizeof(text));
            EXPECT_STR(text, "On \xC2\xB7 50%");
            ha_dash_format_state(temp, text, sizeof(text));
            EXPECT_STR(text, "21.5");
            EXPECT_STR(temp->unit, "°C");
            assert(!ha_dash_tile_is_active(temp));
            EXPECT_STR(ha_dash_icon_glyph(temp), "\xEF\x8B\x89"); /* thermometer */
            ha_dash_format_state(door, text, sizeof(text));
            EXPECT_STR(text, "Open");

            /* Unchanged state: no revision bump. */
            assert(!apply("{\"c\":{\"sensor.temp\":{\"+\":{\"lc\":2}}}}"));

            assert(apply("{\"c\":{\"light.sofa\":{\"+\":{\"s\":\"off\",\"lc\":3},\"-\":{\"a\":[\"brightness\"]}}}}"));
            assert(light->brightness_pct == -1 && light->revision == 2);
            ha_dash_format_state(light, text, sizeof(text));
            EXPECT_STR(text, "Off");

            char msg[256];
            assert(ha_dash_build_action(light, HA_DASH_ACTION_TAP, 0, 7, msg, sizeof(msg)) > 0);
            EXPECT_STR(msg, "{\"id\":7,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"toggle\","
                            "\"target\":{\"entity_id\":\"light.sofa\"}}");
            assert(ha_dash_build_action(light, HA_DASH_ACTION_SET_LEVEL, 40, 8, msg, sizeof(msg)) > 0);
            EXPECT_STR(msg, "{\"id\":8,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
                            "\"target\":{\"entity_id\":\"light.sofa\"},\"service_data\":{\"brightness_pct\":40}}");
            assert(ha_dash_build_action(light, HA_DASH_ACTION_SET_LEVEL, 0, 9, msg, sizeof(msg)) > 0);
            assert(strstr(msg, "\"service\":\"turn_off\""));
            assert(ha_dash_build_action(lock, HA_DASH_ACTION_TAP, 0, 10, msg, sizeof(msg)) > 0);
            assert(strstr(msg, "\"domain\":\"lock\",\"service\":\"unlock\""));
            assert(ha_dash_build_action(&snap.tiles[4], HA_DASH_ACTION_TAP, 0, 11, msg, sizeof(msg)) > 0);
            assert(strstr(msg, "\"domain\":\"scene\",\"service\":\"turn_on\""));
            assert(ha_dash_build_action(temp, HA_DASH_ACTION_TAP, 0, 12, msg, sizeof(msg)) == 0);
            assert(ha_dash_build_action(light, HA_DASH_ACTION_TAP, 0, 13, msg, 20) == -1);

            assert(apply("{\"r\":[\"lock.front\"]}"));
            assert(ha_dash_tile_is_unavailable(lock));
            ha_dash_format_state(lock, text, sizeof(text));
            EXPECT_STR(text, "Unavailable");
            return 0;
        }
        """)

    def test_long_utf8_names_are_cut_on_character_boundaries(self):
        self.run_harness(r"""
        int main(void)
        {
            /* 46 ASCII bytes, then "åäö": the 47-byte name buffer must not end mid-character. */
            parse("{\"views\":[{\"cards\":[{\"type\":\"tile\",\"entity\":\"switch.x\","
                  "\"name\":\"0123456789012345678901234567890123456789012345åäö\"}]}]}");
            const char *name = snap.tiles[0].name;
            assert(strlen(name) == 46);
            return 0;
        }
        """)

    def test_icon_keywords_prefer_specific_words(self):
        self.run_harness(r"""
        int main(void)
        {
            ha_dash_tile_t t;
            memset(&t, 0, sizeof(t));
            strcpy(t.entity_id, "sensor.x");
            strcpy(t.icon, "mdi:lightning-bolt");
            EXPECT_STR(ha_dash_icon_glyph(&t), "\xEF\x83\xA7"); /* bolt, not bulb */
            strcpy(t.icon, "mdi:lock-open-variant");
            EXPECT_STR(ha_dash_icon_glyph(&t), "\xEF\x8F\x81"); /* unlocked */
            strcpy(t.icon, "");
            strcpy(t.entity_id, "fan.bedroom");
            EXPECT_STR(ha_dash_icon_glyph(&t), "\xEF\x9C\xAE"); /* wind */
            return 0;
        }
        """)

    def test_websocket_url_normalization(self):
        self.run_harness(r"""
        int main(void)
        {
            char url[96];
            assert(ha_dash_websocket_url("192.168.1.10", url, sizeof(url)));
            EXPECT_STR(url, "ws://192.168.1.10:8123/api/websocket");
            assert(ha_dash_websocket_url("homeassistant.local:8124", url, sizeof(url)));
            EXPECT_STR(url, "ws://homeassistant.local:8124/api/websocket");
            assert(ha_dash_websocket_url("http://ha.lan:8123/", url, sizeof(url)));
            EXPECT_STR(url, "ws://ha.lan:8123/api/websocket");
            assert(ha_dash_websocket_url("https://example.ui.nabu.casa", url, sizeof(url)));
            EXPECT_STR(url, "wss://example.ui.nabu.casa/api/websocket");
            assert(ha_dash_websocket_url("ws://10.0.0.2:8123/api/websocket", url, sizeof(url)));
            EXPECT_STR(url, "ws://10.0.0.2:8123/api/websocket");
            assert(!ha_dash_websocket_url("", url, sizeof(url)));
            assert(!ha_dash_websocket_url("http://", url, sizeof(url)));
            assert(!ha_dash_websocket_url("192.168.1.10", url, 16));
            return 0;
        }
        """)

    def test_dashboard_list_appends_the_default_dashboard(self):
        self.run_harness(r"""
        int main(void)
        {
            cJSON *root = cJSON_Parse("[{\"id\":\"map\",\"title\":\"Map\",\"url_path\":\"map\",\"icon\":\"mdi:map\"},"
                                      "{\"id\":\"t\",\"url_path\":\"touch-screen\"},{\"id\":\"broken\"}]");
            ha_dash_board_t boards[3];
            int n = ha_dash_parse_dashboards(root, boards, 3);
            assert(n == 3);
            EXPECT_STR(boards[0].title, "Map");
            EXPECT_STR(boards[0].icon, "mdi:map");
            EXPECT_STR(boards[1].title, "touch-screen"); /* no title: fall back to the path */
            EXPECT_STR(boards[2].url_path, "");
            EXPECT_STR(boards[2].title, "Default dashboard");
            /* The default dashboard always fits, even when the list is full. */
            assert(ha_dash_parse_dashboards(root, boards, 2) == 2);
            EXPECT_STR(boards[1].url_path, "");
            cJSON_Delete(root);
            return 0;
        }
        """)

    def test_auth_url_helpers(self):
        self.run_harness(r"""
        int main(void)
        {
            char out[96];
            assert(ha_dash_http_base("ws://192.168.1.10:8123/api/websocket", out, sizeof(out)));
            EXPECT_STR(out, "http://192.168.1.10:8123");
            assert(ha_dash_http_base("wss://example.ui.nabu.casa/api/websocket", out, sizeof(out)));
            EXPECT_STR(out, "https://example.ui.nabu.casa");
            assert(!ha_dash_http_base("ws://host:8123/api/websocket", out, 10));
            ha_dash_server_label("ws://192.168.1.10:8123/api/websocket", out, sizeof(out));
            EXPECT_STR(out, "192.168.1.10:8123");
            ha_dash_server_label("", out, sizeof(out));
            EXPECT_STR(out, "");

            assert(ha_dash_url_encode("http://sensecap-indicator.local/", out, sizeof(out)) > 0);
            EXPECT_STR(out, "http%3A%2F%2Fsensecap-indicator.local%2F");
            assert(ha_dash_url_encode("a b&c=d~", out, sizeof(out)) > 0);
            EXPECT_STR(out, "a%20b%26c%3Dd~");
            assert(ha_dash_url_encode("åäö", out, 4) == -1);
            return 0;
        }
        """)


if __name__ == "__main__":
    unittest.main()
