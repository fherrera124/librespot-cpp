# Enable testing
if(NOT BELL_DISABLE_TESTS)
    # Trompeloeil for mocking
    add_subdirectory(external/trompeloeil)
    # Doctest for unit testing
    add_subdirectory(external/doctest)
    enable_testing()
    add_subdirectory(test)
endif()

# All includes are referenced from the root directory as "bell/"
list(APPEND BELL_INCLUDES "include")

# Main library sources
file(GLOB BELL_SOURCES
    "main/io/*.cpp" # bell::io
    "main/net/*.cpp" # bell::net
    "main/http/*.cpp" # bell::http
    "main/utils/*.cpp" # bell::utils
    "main/audio/*.cpp" # bell::audio
    "main/dsp/*.cpp" # bell::dsp
)

# BELL_CODEC_AAC/BELL_CODEC_OPUS only gate whether the third-party fdk-aac/
# opus subdirectories get added below (and thus their headers/libs) - the
# glob above still picks up bell's own AACCodec.cpp/OpusCodec.cpp wrappers
# regardless, which #include those libraries' headers directly and fail to
# compile once the option is off.
if(NOT BELL_CODEC_AAC)
    list(REMOVE_ITEM BELL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main/audio/AACCodec.cpp")
endif()
if(NOT BELL_CODEC_OPUS)
    list(REMOVE_ITEM BELL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main/audio/OpusCodec.cpp")
endif()
if(BELL_DISABLE_MQTT)
    list(REMOVE_ITEM BELL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main/net/MQTTClient.cpp")
endif()

# Add platform-specific sources
if(APPLE)
    file(GLOB BELL_SOURCES_APPLE "main/platform/apple/*.cpp")
    list(APPEND BELL_SOURCES ${BELL_SOURCES_APPLE})
endif()

# Unix common includes
if(UNIX)
    file(GLOB BELL_SOURCES_POSIX "main/platform/posix/*.cpp")
    list(APPEND BELL_SOURCES ${BELL_SOURCES_POSIX})
endif()

# Linux includes
if (UNIX AND NOT APPLE)
    file(GLOB BELL_SOURCES_POSIX "main/platform/linux/*.cpp")
    list(APPEND BELL_SOURCES ${BELL_SOURCES_POSIX})
endif()

# Espressif includes
if (ESP_PLATFORM)
    file(GLOB BELL_SOURCES_ESP "main/platform/esp/*.cpp" "main/platform/esp/*.S")
    list(APPEND BELL_SOURCES ${BELL_SOURCES_ESP})
endif()
