# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# librespot-cpp itself (this repo's own root CMakeLists.txt, which calls
# idf_component_register() when ESP_PLATFORM is set - see that file's own
# comment) plus protocol_examples_common (example_connect(), the same
# WiFi-bootstrap helper this repo's own feature/esp32-port branch uses).
# The parent path is this checkout's repo root, two directories up from
# here - if you've copied just extras/esp32/ out on its own, point this
# at wherever you cloned librespot-cpp instead.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common"
)

# sdkconfig.defaults.local (gitignored, not tracked) carries this
# machine's real WiFi SSID/password for example_connect() - never
# committed. Falls back to just sdkconfig.defaults (ESP-IDF's own
# placeholder myssid/mypassword) if the local file doesn't exist yet.
# Same convention as this repo's own feature/esp32-port branch
# (targets/esp32/CMakeLists.txt).
set(_sdkconfig_defaults "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND _sdkconfig_defaults "sdkconfig.defaults.local")
endif()
set(SDKCONFIG_DEFAULTS "${_sdkconfig_defaults}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(librespot-cpp-esp32-example)
