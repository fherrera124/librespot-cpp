set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    INCLUDE_DIRS "../../cspot/include"
    REQUIRES esp_driver_i2s
    PRIV_REQUIRES esp_ringbuf
)

add_compile_options(-Wno-unused-variable -Wno-unused-const-variable
                     -Wchar-subscripts -Wunused-label -Wmaybe-uninitialized
                     -Wmisleading-indentation)

# Kconfig -> variables CMake que bell/CMakeLists.txt ya entiende (option()),
# shadow por set() plano antes del add_subdirectory().
set(BELL_CODEC_VORBIS ${CONFIG_LIBRESPOT_CODEC_VORBIS})
set(BELL_CODEC_MP3 ${CONFIG_LIBRESPOT_CODEC_MP3})
set(BELL_CODEC_AAC ${CONFIG_LIBRESPOT_CODEC_AAC})
set(BELL_CODEC_OPUS ${CONFIG_LIBRESPOT_CODEC_OPUS})
set(BELL_CODEC_ALAC ${CONFIG_LIBRESPOT_CODEC_ALAC})
if(NOT CONFIG_LIBRESPOT_ENABLE_MQTT)
    set(BELL_DISABLE_MQTT ON)
endif()
if(NOT CONFIG_LIBRESPOT_ENABLE_WEBSERVER)
    set(BELL_DISABLE_WEBSERVER ON)
endif()
if(NOT CONFIG_LIBRESPOT_USE_NLOHMANN_JSON)
    set(BELL_ONLY_CJSON ON)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../cspot ${CMAKE_CURRENT_BINARY_DIR}/cspot)
target_link_libraries(${COMPONENT_LIB} PRIVATE cspot)
target_link_libraries(bell PRIVATE idf::esp_driver_i2s idf::esp_ringbuf)

# tremor (Vorbis vendorizado) tropieza con -Werror en este toolchain.
file(GLOB _tremor_srcs "${CMAKE_CURRENT_SOURCE_DIR}/../../cspot/bell/external/tremor/*.c")
set_property(SOURCE ${_tremor_srcs}
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../cspot/bell
    PROPERTY COMPILE_OPTIONS "-Wno-error")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
