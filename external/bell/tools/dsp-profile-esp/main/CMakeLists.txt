# Find all source files with
file(GLOB COMPONENT_SRCS "main.cpp")

set(COMPONENT_ADD_INCLUDEDIRS
    "."
)

set(COMPONENT_REQUIRES
    nvs_flash
    driver
    esp_netif
    lwip
    esp_timer
    esp_wifi
    bell)
register_component()

component_compile_options(-std=gnu++20 -Wno-format -Wformat-truncation -Wrestrict -Wswitch -fdiagnostics-color=always)