cmake_minimum_required(VERSION 3.20)
project(cyd_sim LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBDEPS      ${PROJECT_ROOT}/.pio/libdeps/cyd)

# Pull in LVGL from the PlatformIO library cache and use the project's lv_conf.h.
set(LV_BUILD_CONF_DIR ${PROJECT_ROOT}/include CACHE STRING "" FORCE)
add_subdirectory(${LIBDEPS}/lvgl ${CMAKE_BINARY_DIR}/lvgl EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
# Use pkg-config for includes and libdir. Link SDL2 explicitly via linker flag
# (-lSDL2) so we use the import library (libSDL2.dll.a) and skip SDL2main,
# which on Windows hijacks main() via SDL_main.
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(cyd_sim
    sim_main.cpp
    sim_lvgl_bridge.cpp
    fake_data.cpp
    arduino_compat.cpp
    ${PROJECT_ROOT}/src/net/quote_store.cpp
    ${PROJECT_ROOT}/src/settings/settings_store.cpp
    ${PROJECT_ROOT}/src/ui/styles.cpp
    ${PROJECT_ROOT}/src/ui/logos.cpp
    ${PROJECT_ROOT}/src/ui/logos_data.cpp
    ${PROJECT_ROOT}/src/ui/list_screen.cpp
    ${PROJECT_ROOT}/src/ui/detail_screen.cpp
    ${PROJECT_ROOT}/src/ui/settings_screen.cpp
    ${PROJECT_ROOT}/src/ui/wifi_setup_screen.cpp
    wifi_mgr_stub.cpp
)

target_compile_definitions(cyd_sim PRIVATE
    SIM_BUILD=1
    LV_CONF_INCLUDE_SIMPLE
    ARDUINOJSON_ENABLE_ARDUINO_STRING=1
    SDL_MAIN_HANDLED
    "LV_MEM_SIZE=(512U*1024U)"
)

# Order matters: the sim/compat/ shim headers must shadow Arduino-ESP32's.
target_include_directories(cyd_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/src
    ${LIBDEPS}/ArduinoJson/src
)

target_include_directories(cyd_sim PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(cyd_sim    PRIVATE ${SDL2_LIBRARY_DIRS})

target_link_libraries(cyd_sim PRIVATE
    lvgl::lvgl
    # Use the flag form so the linker resolves libSDL2.dll.a (not SDL2.lib).
    "-lSDL2"
)
