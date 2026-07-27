idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    # librespot-cpp: this repo's own root CMakeLists.txt (idf_component_register()
    # under ESP_PLATFORM), discovered via the parent CMakeLists.txt's
    # EXTRA_COMPONENT_DIRS - the component name ESP-IDF derives is this
    # checkout's own directory name. Rename this entry to match if you've
    # cloned it under a different name.
    REQUIRES librespot-cpp esp_driver_i2s esp_event esp_netif esp_wifi
             nvs_flash protocol_examples_common
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
