cmake_minimum_required(VERSION 3.10)

# Lets this repo be fetched directly as an ESP-IDF managed component (a
# downstream project's idf_component.yml pointing its `git:` dependency at
# this repo's root - see idf_component.yml alongside this file). Guarded
# on the CSPOT_TARGET_* options below staying off: none of them are ever
# set by a component-manager fetch (the fetching project has no way to
# pass them in), only by this repo's own local dev/test workflow
# (CSPOT_TARGET_ESP32 for targets/esp32's own build, etc.) - so this
# never runs alongside those. Reuses targets/esp32/main/CMakeLists.txt's
# already-proven recipe verbatim, one directory level up: main/
# CMakeLists.txt already handles ESP_PLATFORM internally (the mbedtls
# collision workaround), so there's nothing to duplicate here beyond the
# same idf_component_register()/warning-suppression/link wiring.
if(ESP_PLATFORM AND NOT CSPOT_TARGET_ESP32 AND NOT CSPOT_TARGET_TESTS)
    idf_component_register(
        PRIV_REQUIRES esp_driver_i2s esp_event esp_netif esp_wifi nvs_flash spiffs
    )
    idf_build_set_property(COMPILE_OPTIONS "-Wno-error=deprecated-literal-operator" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-error=implicit-fallthrough" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-error=template-id-cdtor" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

    option(BUILD_SHARED_LIBS OFF)
    option(BUILD_TESTING OFF)

    add_subdirectory(main ${CMAKE_CURRENT_BINARY_DIR}/cspot)

    target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
    target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
    return()
endif()

# Configurable options
option(CSPOT_TARGET_CLI "Enable cspotcli target" OFF)
option(CSPOT_TARGET_ESP32 "Enable cspot-esp32 target" OFF)
option(CSPOT_TARGET_TESTS "Enable cspot tests target" OFF)

# Enable cspotcli target
if(CSPOT_TARGET_CLI)
    add_subdirectory(targets/cli)
    project(cspotcli)
endif()

# Enable cspot-esp32 target
if(CSPOT_TARGET_ESP32)
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    set(IDF_NO_INCLUDE ON)
    add_subdirectory(targets/esp32)
    project(cspot-esp32)
endif()

if(CSPOT_TARGET_TESTS)
    add_subdirectory(main)
    project(cspot_tests)
endif()
