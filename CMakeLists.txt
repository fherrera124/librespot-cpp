cmake_minimum_required(VERSION 2.8.12)

# Configurable options
option(CSPOT_TARGET_CLI "Enable cspotcli target" OFF)

# Enable cspotcli target
if(CSPOT_TARGET_CLI)
    add_subdirectory(targets/cli)
    project(cspotcli)
endif()

# ESP-IDF consumption goes through cspot/CMakeLists.txt directly (dual-mode:
# idf_component_register() under ESP_PLATFORM, plain add_library() here) -
# see cspot/README.md. No separate esp32 target: Component Manager's `path:`
# for git dependencies only materializes that one subdirectory, so the
# wrapper has to live where cspot/bell's own sources already are, not in a
# sibling directory reaching outside its own subtree.

