cmake_minimum_required(VERSION 3.5)

# Main target sources
file(GLOB SOURCES "*.cpp" "*.c")

# Not wired to the new Session/api architecture yet (this is the Stage C
# checkpoint - proving Session/api/tracks + bell compile for ESP32, no
# audio/status-LED wiring yet), and its header unconditionally includes
# led_strip.h, whose component moved to the ESP Component Registry in
# newer ESP-IDF (no longer bundled under examples/common_components/).
# Revisit when status LED support actually gets wired up.
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ESPStatusLed.cpp")

# Configure the target
# Specifying REQUIRES/PRIV_REQUIRES at all (needed for esp_driver_i2s, the
# new AudioSinkI2S dependency) turns off ESP-IDF's legacy fallback of
# linking against every component in the build - main.cpp's own direct
# esp_event.h/esp_spiffs.h/esp_wifi.h/nvs_flash.h/protocol_examples_common.h
# includes (previously resolved "for free" via that fallback) need to be
# listed explicitly too.
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2s esp_event esp_netif esp_wifi nvs_flash spiffs protocol_examples_common
)
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)

# taojson (bell's vendored JSON lib, used throughout Session/api) still uses
# the pre-C++23 `operator"" _foo()` syntax (space before the suffix) - this
# toolchain's GCC (15.2) flags it as deprecated under the C++ standard it
# defaults to, and -Werror turns that into a hard build failure. Not
# taojson's own bug to fix here (third-party vendored header), so suppressed
# build-wide like the line above.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=deprecated-literal-operator" APPEND)

# yajl (cspot's vendored C JSON parser, used by tracks/ContextPageParser.cpp)
# has a real switch-fallthrough this toolchain's GCC now flags by default -
# same category as the taojson suppression above, third-party vendored code.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=implicit-fallthrough" APPEND)

# websocketpp (DealerClient's WS transport) still uses the pre-C++20
# `ClassName<T>()` constructor/destructor template-id syntax GCC now flags
# under -std=c++20 - same category, third-party vendored code, harmless
# warning on the host build's older GCC but this toolchain's GCC 15.2
# escalates it via -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=template-id-cdtor" APPEND)

# TrackQueueHandler.cpp's designated initializers for cspot_proto::ProvidedTrack
# (a nanopb-generated struct) deliberately leave fields like `gid` at their
# default zero-init - standard, correct C++20 designated-initializer
# behavior, but this toolchain's GCC flags any unlisted field regardless.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

# Build static library, do not build test executables
option(BUILD_SHARED_LIBS OFF)
option(BUILD_TESTING OFF)

# Import cspot library (lives in main/ - project(cspot) is just its CMake
# project name, not its directory)
add_subdirectory("../../../main" ${CMAKE_CURRENT_BINARY_DIR}/cspot)

# Configure the target
target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
