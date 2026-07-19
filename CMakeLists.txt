cmake_minimum_required(VERSION 2.8.12)

# Configurable options
option(CSPOT_TARGET_CLI "Enable cspotcli target" OFF)

# Enable cspotcli target
if(CSPOT_TARGET_CLI)
    add_subdirectory(targets/cli)
    project(cspotcli)
endif()

# targets/esp32 is a real ESP-IDF component (idf_component_register()),
# consumed via the Component Manager by a downstream project - it can't be
# built standalone as a top-level project() the way CSPOT_TARGET_CLI can.

