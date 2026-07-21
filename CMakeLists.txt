cmake_minimum_required(VERSION 2.8.12)

# cmake_minimum_required(2.8.12) leaves CMP0079 OLD for this directory
# scope, which blocks target_link_libraries(bell ...) below (bell is
# add_subdirectory()'d, a different directory than this one).
if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

# ESP-IDF (Component Manager) vs plain CMake (CLI target, host tests):
# idf_component_register() must run before anything touches ${COMPONENT_LIB}.
# Under ESP_PLATFORM, this file is reached directly by ESP-IDF's early
# component-requirements scan, via a restricted include() where project()
# isn't allowed ("project command is not scriptable") - so project() is
# only called in the plain-CMake branch, where nothing else guarantees an
# ancestor has called it first (unlike before this repo was flattened,
# when only targets/cli/CMakeLists.txt reached this file, and it always
# called project() itself before doing so).
if(NOT ESP_PLATFORM)
    project(cspot)
endif()

file(GLOB SOURCES "src/*.cpp" "src/*.c")
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_i2s
        PRIV_REQUIRES esp_ringbuf
    )
endif()

# Configurable options
set(CSPOT_EXTERNAL_BELL "" CACHE STRING "External bell library target name, optional")
option(CSPOT_TARGET_CLI "Enable cspotcli target" OFF)

# CMake options
set(CMAKE_CXX_STANDARD 20)

if(MSVC)
	add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
	add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142 /wd4996)
endif()

if(ESP_PLATFORM)
    add_compile_options(-Wno-unused-variable -Wno-unused-const-variable
                         -Wchar-subscripts -Wunused-label -Wmaybe-uninitialized
                         -Wmisleading-indentation)

    # Kconfig -> variables CMake que bell/CMakeLists.txt ya entiende
    # (option()), shadow por set() plano antes del add_subdirectory().
    # CONFIG_X no existe como variable en absoluto cuando el Kconfig bool
    # está en "n" (no es que valga vacío/OFF) - set(VAR ${CONFIG_X}) en ese
    # caso equivale a set(VAR), que directamente NO define BELL_CODEC_*,
    # dejando que el option() ON por defecto de bell/CMakeLists.txt gane.
    # if/else explícito evita ese caso.
    if(CONFIG_LIBRESPOT_CODEC_VORBIS)
        set(BELL_CODEC_VORBIS ON)
    else()
        set(BELL_CODEC_VORBIS OFF)
    endif()
    if(CONFIG_LIBRESPOT_CODEC_MP3)
        set(BELL_CODEC_MP3 ON)
    else()
        set(BELL_CODEC_MP3 OFF)
    endif()
    if(CONFIG_LIBRESPOT_CODEC_AAC)
        set(BELL_CODEC_AAC ON)
    else()
        set(BELL_CODEC_AAC OFF)
    endif()
    if(CONFIG_LIBRESPOT_CODEC_OPUS)
        set(BELL_CODEC_OPUS ON)
    else()
        set(BELL_CODEC_OPUS OFF)
    endif()
    if(CONFIG_LIBRESPOT_CODEC_ALAC)
        set(BELL_CODEC_ALAC ON)
    else()
        set(BELL_CODEC_ALAC OFF)
    endif()
    if(NOT CONFIG_LIBRESPOT_ENABLE_MQTT)
        set(BELL_DISABLE_MQTT ON)
    endif()
    if(NOT CONFIG_LIBRESPOT_ENABLE_WEBSERVER)
        set(BELL_DISABLE_WEBSERVER ON)
    endif()
    if(NOT CONFIG_LIBRESPOT_USE_NLOHMANN_JSON)
        set(BELL_ONLY_CJSON ON)
    endif()
endif()

# Use externally specified bell library or the vendored copy
if(CSPOT_EXTERNAL_BELL)
    list(APPEND EXTRA_LIBS ${CSPOT_EXTERNAL_BELL})
else()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/bell)
    list(APPEND EXTRA_LIBS bell)
endif()

# Build protobuf code
set(NANOPB_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB PROTOS protobuf/*.proto)
nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTOS})
add_custom_target(generate_proto_sources DEPENDS ${PROTO_SRCS} ${PROTO_HDRS})
set_source_files_properties(${PROTO_SRCS} ${PROTO_HDRS} PROPERTIES GENERATED TRUE)

if(ESP_PLATFORM)
    target_sources(${COMPONENT_LIB} PRIVATE ${PROTO_SRCS})
    target_compile_definitions(bell PUBLIC PB_ENABLE_MALLOC)
    target_compile_definitions(bell PUBLIC PB_FIELD_32BIT)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${EXTRA_LIBS})
    target_include_directories(${COMPONENT_LIB} PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${NANOPB_INCLUDE_DIRS} ${EXTRA_INCLUDES})
    target_link_libraries(bell PRIVATE idf::esp_driver_i2s idf::esp_ringbuf)

    # tremor (Vorbis vendorizado) tropieza con -Werror en este toolchain.
    file(GLOB _tremor_srcs "${CMAKE_CURRENT_SOURCE_DIR}/external/bell/external/tremor/*.c")
    set_property(SOURCE ${_tremor_srcs}
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/bell
        PROPERTY COMPILE_OPTIONS "-Wno-error")

    target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
else()
    add_library(cspot STATIC ${SOURCES} ${PROTO_SRCS})
    # PUBLIC to propagate includes from bell to cspot dependents
    target_compile_definitions(bell PUBLIC PB_ENABLE_MALLOC)
    target_compile_definitions(bell PUBLIC PB_FIELD_32BIT)
    target_link_libraries(cspot PUBLIC ${EXTRA_LIBS})
    target_include_directories(cspot PUBLIC "include" ${CMAKE_CURRENT_BINARY_DIR} ${NANOPB_INCLUDE_DIRS} ${EXTRA_INCLUDES})
endif()

# Enable cspotcli target - the cspot and bell targets above are already defined
# by the time we get here, so extras/cli just links against them directly
# (no more reaching sideways into a sibling directory's CMakeLists.txt).
if(CSPOT_TARGET_CLI)
    add_subdirectory(extras/cli)
endif()
