cmake_minimum_required(VERSION 2.8.12)
cmake_policy(SET CMP0077 NEW)

project(bell)

# Configurable options
option(BELL_CODEC_MP3 "Support libhelix-mp3 codec" ON)
option(BELL_DISABLE_SINKS "Disable all built-in audio sink implementations" OFF)

# These are default OFF, as they're OS-dependent (ESP32 sinks are always enabled - no external deps)
option(BELL_SINK_ALSA "Enable ALSA audio sink" OFF)
option(BELL_SINK_PORTAUDIO "Enable PortAudio sink" OFF)

# cJSON wrapper
option(BELL_ONLY_CJSON "Use only cJSON, not Nlohmann")
set(BELL_EXTERNAL_CJSON "" CACHE STRING "External cJSON library target name, optional")

# vorbis
set(BELL_EXTERNAL_VORBIS "" CACHE STRING "External Vorbis library target name, optional")
option(BELL_VORBIS_FLOAT "Use floating point Vorbis API" OFF)

# fmt & regex
option(BELL_DISABLE_FMT "Don't use std::fmt (saves space)" OFF)
option(BELL_DISABLE_REGEX "Don't use std::regex (saves space)" OFF)

# disable json tests
set(JSON_BuildTests OFF CACHE INTERNAL "")

# Backwards compatibility with deprecated options
if(BELL_USE_ALSA)
    message(WARNING "Deprecated Bell options used, replace BELL_USE_ALSA with BELL_SINK_ALSA")
    set(BELL_SINK_ALSA ${BELL_USE_ALSA})
endif()

if(BELL_USE_PORTAUDIO)
    message(WARNING "Deprecated Bell options used, replace BELL_USE_PORTAUDIO with BELL_SINK_PORTAUDIO")
    set(BELL_SINK_PORTAUDIO ${BELL_USE_PORTAUDIO})
endif()

message(STATUS "Bell options:")
message(STATUS "    - Vorbis audio codec: always on (tracks)")
message(STATUS "    - MP3 audio codec: ${BELL_CODEC_MP3} (podcast episodes)")

message(STATUS "    Disable built-in audio sinks: ${BELL_DISABLE_SINKS}")
message(STATUS "    Use Vorbis float version: ${BELL_VORBIS_FLOAT}")

if(NOT BELL_DISABLE_SINKS)
    message(STATUS "    - ALSA sink: ${BELL_SINK_ALSA}")
    message(STATUS "    - PortAudio sink: ${BELL_SINK_PORTAUDIO}")
endif()

message(STATUS "    Use cJSON only: ${BELL_ONLY_CJSON}")
message(STATUS "    Disable Fmt: ${BELL_DISABLE_FMT}")
message(STATUS "    Disable Regex: ${BELL_DISABLE_REGEX}")

# Include nanoPB library
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/external/nanopb/extra")
find_package(Nanopb REQUIRED)
message(${NANOPB_INCLUDE_DIRS})
list(APPEND EXTERNAL_INCLUDES ${NANOPB_INCLUDE_DIRS})

# CMake options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(AUDIO_CODEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main/audio-codec")
set(AUDIO_SINKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main/audio-sinks")

add_definitions("-DUSE_DEFAULT_STDLIB=1 -DTARGET_OS_IPHONE=0")

# Main library sources
file(GLOB SOURCES
    "external/nanopb/*.c"
    "main/utilities/*.cpp" "main/utilities/*.c"
    "main/io/*.cpp" "main/io/*.c"
)

list(APPEND EXTRA_INCLUDES "main/audio-codec/include")
list(APPEND EXTRA_INCLUDES "main/audio-dsp/include")
list(APPEND EXTRA_INCLUDES "main/audio-sinks/include")
list(APPEND EXTRA_INCLUDES "main/io/include")
list(APPEND EXTRA_INCLUDES "main/utilities/include")
list(APPEND EXTRA_INCLUDES "main/platform")

# Add platform specific sources
if(ESP_PLATFORM)
    file(GLOB ESP_PLATFORM_SOURCES "main/platform/esp/*.cpp" "main/platform/esp/*.c")
    list(APPEND SOURCES ${ESP_PLATFORM_SOURCES})
endif()

if(APPLE)
    file(GLOB APPLE_PLATFORM_SOURCES "main/platform/apple/*.cpp" "main/platform/apple/*.c")
    list(APPEND SOURCES ${APPLE_PLATFORM_SOURCES})
    list(APPEND EXTERNAL_INCLUDES "/usr/local/opt/mbedtls@3/include")
endif()

if(UNIX AND NOT APPLE)
    file(GLOB LINUX_PLATFORM_SOURCES "main/platform/linux/*.cpp" "main/platform/linux/*.c")
    list(APPEND SOURCES ${LINUX_PLATFORM_SOURCES})
endif()

if(WIN32)
    file(GLOB WIN32_PLATFORM_SOURCES "main/platform/win32/*.cpp" "main/platform/win32/*.c")
    list(APPEND SOURCES ${WIN32_PLATFORM_SOURCES})
    list(APPEND EXTERNAL_INCLUDES "main/platform/win32")
endif()

if(ESP_PLATFORM)
    if (IDF_VERSION_MAJOR LESS_EQUAL 4)
        list(APPEND EXTRA_LIBS idf::mdns idf::mbedtls idf::pthread idf::driver idf::lwip)
    else()	
        list(APPEND EXTRA_LIBS idf::espressif__mdns idf::mbedtls idf::pthread idf::driver idf::lwip)
    endif()
    add_definitions(-Wunused-const-variable -Wchar-subscripts -Wunused-label -Wmaybe-uninitialized -Wmisleading-indentation -Wno-stringop-overflow -Wno-error=format -Wno-format -Wno-stringop-overread -Wno-stringop-overflow)
else()
    find_package(Threads REQUIRED)
    find_package(MbedTLS REQUIRED)
    list(APPEND EXTERNAL_INCLUDES ${MBEDTLS_INCLUDE_DIRS})
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    list(APPEND EXTRA_LIBS ${MBEDTLS_LIBRARIES} Threads::Threads)

    if(MSVC)
        add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
        add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142)
    endif()
endif()

# MP3 codec (podcast episodes) - the only optional codec left. Vorbis
# (tracks) is unconditional, compiled below regardless of this option -
# there's no real "disable Vorbis" use case, it'd break basic playback.
if(BELL_CODEC_MP3)
    file(GLOB LIBHELIX_MP3_SOURCES "external/libhelix-mp3/*.c")
    list(APPEND LIBHELIX_SOURCES ${LIBHELIX_MP3_SOURCES})
    list(APPEND EXTERNAL_INCLUDES "external/libhelix-mp3")
    list(APPEND SOURCES "${AUDIO_CODEC_DIR}/MP3Decoder.cpp" "${AUDIO_CODEC_DIR}/BaseCodec.cpp")
    list(APPEND EXTRA_INCLUDES "main/audio-containers/include")

    # libhelix Cygwin workaround
    if(CYGWIN)
        # Both Cygwin and ESP are Unix-like so this seems to work (or, at least, compile)
        set_source_files_properties(${LIBHELIX_SOURCES} PROPERTIES COMPILE_FLAGS "-DESP_PLATFORM")
    endif()

    list(APPEND SOURCES ${LIBHELIX_SOURCES})
endif()

if(NOT BELL_EXTERNAL_VORBIS STREQUAL "")
    message(STATUS "Using external Vorbis codec ${BELL_EXTERNAL_VORBIS}")
    list(APPEND EXTRA_LIBS ${BELL_EXTERNAL_VORBIS})
else()  
    file(GLOB TREMOR_SOURCES "external/tremor/*.c")
    list(REMOVE_ITEM TREMOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/external/tremor/ivorbisfile_example.c")
    list(APPEND SOURCES ${TREMOR_SOURCES})
    list(APPEND EXTERNAL_INCLUDES "external/tremor")
endif()

if(NOT BELL_DISABLE_SINKS)
    set(PLATFORM "unix")

    if(ESP_PLATFORM)
        set(PLATFORM "esp")
    endif()

    # Add all built-in audio sinks - only BufferedAudioSink (shared I2S
    # base) and PlainI2SAudioSink exist for ESP32 (AC101/ES8311/ES8388/
    # ES9018/TAS5711/SPDIF/InternalAudioSink used the legacy `driver/i2s.h`
    # API removed in ESP-IDF v5.4+/v6 and were never ported - deleted
    # rather than left uncompilable).
    file(GLOB SINK_SOURCES "${AUDIO_SINKS_DIR}/${PLATFORM}/*.cpp" "${AUDIO_SINKS_DIR}/${PLATFORM}/*.c")

    list(APPEND EXTRA_INCLUDES "main/audio-sinks/include/${PLATFORM}")

    # Find ALSA if required, else remove the sink
    if(BELL_SINK_ALSA)
        find_package(ALSA REQUIRED)
        list(APPEND EXTERNAL_INCLUDES ${ALSA_INCLUDE_DIRS})
        list(APPEND EXTRA_LIBS ${ALSA_LIBRARIES})
    else()
        list(REMOVE_ITEM SINK_SOURCES "${AUDIO_SINKS_DIR}/unix/ALSAAudioSink.cpp")
    endif()

    # Find PortAudio if required, else remove the sink
    if(BELL_SINK_PORTAUDIO)
        find_package(Portaudio REQUIRED)
        list(APPEND EXTERNAL_INCLUDES ${PORTAUDIO_INCLUDE_DIRS})
        list(APPEND EXTRA_LIBS ${PORTAUDIO_LIBRARIES})
    else()
        list(REMOVE_ITEM SINK_SOURCES "${AUDIO_SINKS_DIR}/unix/PortAudioSink.cpp")
    endif()

    list(APPEND SOURCES ${SINK_SOURCES})
endif()

if(NOT BELL_ONLY_CJSON)
    set(JSON_SystemInclude ON CACHE INTERNAL "")
    add_subdirectory(external/nlohmann_json)
    list(APPEND EXTRA_LIBS nlohmann_json::nlohmann_json)
endif()	

if(BELL_EXTERNAL_CJSON)
	list(APPEND EXTRA_LIBS ${BELL_EXTERNAL_CJSON})
else()	
	list(APPEND SOURCES "external/cJSON/cJSON.c")
  list(APPEND EXTERNAL_INCLUDES "external/cJSON")
endif()	

if (NOT BELL_DISABLE_FMT)
  list(APPEND EXTERNAL_INCLUDES "external/fmt/include")
endif()

if(WIN32 OR UNIX)
    list(APPEND SOURCES "external/mdnssvc/mdns.c" "external/mdnssvc/mdnsd.c")
    list(APPEND EXTERNAL_INCLUDES "external/mdnssvc")
endif() 

add_library(bell STATIC ${SOURCES})

# Add Apple Bonjour compatibility library for Linux
if(UNIX AND NOT APPLE)
    if (BELL_DISABLE_AVAHI)
        add_compile_definitions(BELL_DISABLE_AVAHI)
    else()
        list(APPEND EXTRA_LIBS avahi-client avahi-common)
    endif()
endif()

# PUBLIC to propagate esp-idf includes to bell dependents
target_link_libraries(bell PUBLIC ${EXTRA_LIBS})
target_include_directories(bell PUBLIC ${EXTRA_INCLUDES} ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(bell SYSTEM PUBLIC ${EXTERNAL_INCLUDES})
target_compile_definitions(bell PUBLIC PB_ENABLE_MALLOC FMT_HEADER_ONLY)

if(BELL_VORBIS_FLOAT)
    target_compile_definitions(bell PUBLIC BELL_VORBIS_FLOAT)
endif() 

if(BELL_DISABLE_FMT)
	target_compile_definitions(bell PUBLIC BELL_DISABLE_FMT)
endif()

if(BELL_DISABLE_REGEX)
    target_compile_definitions(bell PUBLIC BELL_DISABLE_REGEX)
endif()	

if(BELL_ONLY_CJSON)
    target_compile_definitions(bell PUBLIC BELL_ONLY_CJSON)
endif()	

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    target_compile_definitions(bell PUBLIC PB_NO_STATIC_ASSERT)
endif()

