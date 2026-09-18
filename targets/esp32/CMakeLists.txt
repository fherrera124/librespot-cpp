# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Don't override EXTRA_COMPONENT_DIRS as platformio uses it.  Instead we append
# see https://github.com/platformio/platform-espressif32/issues/341
list(APPEND EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

set(BELL_CODEC_AAC OFF CACHE BOOL "Unused by cspot's Session/api/tracks" FORCE)
set(BELL_CODEC_OPUS OFF CACHE BOOL "Unused by cspot's Session/api/tracks" FORCE)
set(BELL_CODEC_HQLC OFF CACHE BOOL "Unused by cspot's Session/api/tracks" FORCE)
set(BELL_CODEC_LC3PLUS OFF CACHE BOOL "Unused by cspot's Session/api/tracks" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "Unused by cspot's Session/api/tracks" FORCE)

# Honor SDKCONFIG_DEFAULTS and append optional sdkconfig.defaults.local.
if(DEFINED SDKCONFIG_DEFAULTS)
    set(_sdkconfig_defaults "${SDKCONFIG_DEFAULTS}")
elseif(DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(_sdkconfig_defaults "$ENV{SDKCONFIG_DEFAULTS}")
else()
    set(_sdkconfig_defaults "sdkconfig.defaults")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND _sdkconfig_defaults "sdkconfig.defaults.local")
endif()
list(REMOVE_DUPLICATES _sdkconfig_defaults)
set(SDKCONFIG_DEFAULTS "${_sdkconfig_defaults}")

if(NOT IDF_NO_INCLUDE)
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
endif()
project(cspot-esp32)
