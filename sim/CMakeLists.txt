cmake_minimum_required(VERSION 3.20)
project(sensecap_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Root of the firmware repo (parent of this sim/ directory)
set(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

include(FetchContent)
FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2
    GIT_SHALLOW    TRUE
)
# Must be set before FetchContent_MakeAvailable so LVGL's CMake picks it up
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(lvgl)

# LVGL's SDL2 driver files need SDL2 in their include path too
target_include_directories(lvgl PRIVATE ${SDL2_INCLUDE_DIRS})

# LVGL 9 globs all *.S files (ARM Helium, NEON) that won't assemble on macOS.
# Mark every .S file as header-only so CMake skips them for this target.
file(GLOB_RECURSE LVGL_ASM_FILES "${lvgl_SOURCE_DIR}/src/*.S")
if(LVGL_ASM_FILES)
    set_source_files_properties(
        ${LVGL_ASM_FILES}
        DIRECTORY "${lvgl_SOURCE_DIR}"
        PROPERTIES HEADER_FILE_ONLY TRUE
    )
endif()

# ── Stub sources ──────────────────────────────────────────────────────────────
# esp_event_stub.c provides the synchronous event dispatch implementation.
# display_model_stub / sensor_model_stub replace hardware model functions.
set(STUB_SOURCES
    stubs/esp_event_stub.c
    stubs/display_model_stub.c
    stubs/sensor_model_stub.c
    stubs/indicator_util_stub.c
    stubs/ha_dash_stub.c
)

# ── UI sources (hardware-free view layer only) ────────────────────────────────
# Do NOT include indicator_view.c (pulls indicator_enabler.h → all hardware).
# Do NOT include model/*.c files (hardware-coupled).
# sim/main.c calls view init functions directly.
file(GLOB ASSET_SOURCES "${FW_ROOT}/main/assets/*.c")

set(UI_SOURCES
    ${FW_ROOT}/main/nav/nav.c
    ${FW_ROOT}/main/display/display_view.c
    ${FW_ROOT}/main/settings/settings_view.c
    ${FW_ROOT}/main/ha/ha_dash_screen.c
    ${FW_ROOT}/main/ha/ha_dash_present.c
    ${FW_ROOT}/main/wifi/wifi_connect_screen.c
    ${FW_ROOT}/main/wifi/wifi_list_screen.c
    ${FW_ROOT}/main/wifi/wifi_view.c
    ${ASSET_SOURCES}
)

# ── Simulator sources ─────────────────────────────────────────────────────────
set(SIM_SOURCES
    main.c
    lv_port_sim.c
    mock/mock_sensors.c
)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(sensecap_sim
    ${STUB_SOURCES}
    ${UI_SOURCES}
    ${SIM_SOURCES}
)

# ── Include paths ─────────────────────────────────────────────────────────────
# IMPORTANT: stubs/ must be first — it shadows ESP-IDF headers
#            (esp_event.h, esp_log.h, freertos/FreeRTOS.h, etc.)
target_include_directories(sensecap_sim PRIVATE
    # 1. Stubs shadow all ESP-IDF / FreeRTOS headers
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    # 2. lv_conf.h lives here (picked up by LVGL via LV_CONF_PATH)
    ${CMAKE_CURRENT_SOURCE_DIR}
    # 3. Firmware source tree — view_data_types.h, home_assistant_config.h, etc.
    ${FW_ROOT}/main
    ${FW_ROOT}/main/nav
    ${FW_ROOT}/main/ha
    ${FW_ROOT}/main/sensor
    ${FW_ROOT}/main/display
    ${FW_ROOT}/main/settings
    ${FW_ROOT}/main/wifi
    ${FW_ROOT}/main/util
    ${FW_ROOT}/main/rp2040
    # 4. SDL2
    ${SDL2_INCLUDE_DIRS}
)

# ── Link ──────────────────────────────────────────────────────────────────────
target_link_directories(sensecap_sim PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(sensecap_sim PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
    m    # sinf in mock_sensors.c
)

target_compile_options(sensecap_sim PRIVATE
    ${SDL2_CFLAGS_OTHER}
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
)
