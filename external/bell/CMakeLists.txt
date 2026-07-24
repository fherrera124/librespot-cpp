cmake_minimum_required(VERSION 3.16)

project(bell)

# CMake options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# Custom build options
include(cmake/BuildOptions.cmake)

# External dependencies
include(cmake/ExternalDependencies.cmake)

# Own sources, tests
include(cmake/LibrarySources.cmake)

add_library(bell STATIC ${BELL_SOURCES})

target_link_libraries(bell PUBLIC ${BELL_LIBS})
target_include_directories(bell PUBLIC ${BELL_INCLUDES} ${BELL_EXTERNAL_INCLUDES} ${CMAKE_CURRENT_BINARY_DIR})

# Set flags
if(BELL_DISABLE_TAOJSON)
    target_compile_definitions(bell PUBLIC BELL_DISABLE_TAOJSON)
endif()

if(BELL_DISABLE_MDNS)
    target_compile_definitions(bell PUBLIC BELL_DISABLE_MDNS)
endif()

# Note: on ESP32, MBEDTLS_ALLOW_PRIVATE_ACCESS (needed for
# mbedtls_md_hmac_*()/aes.h/bignum.h - see DigestCrypto.h and
# CDNDataStream.h/DiffieHellman.h/Authenticator.cpp) is already defined
# unconditionally by ESP-IDF's own mbedtls port config
# (components/mbedtls/port/include/mbedtls/esp_config.h) for every
# consumer - defining it again here collides with mbedTLS's own internal
# sources doing the same. Nothing to set here.

if(BELL_BACKEND_PORTAUDIO)
    target_compile_definitions(bell PUBLIC BELL_BACKEND_PORTAUDIO)
endif()

if(NOT BELL_DISABLE_SANITIZERS)
    # Enable sanitizers
    target_compile_options(bell PUBLIC -fsanitize=address -fsanitize=undefined)
    target_link_options(bell PUBLIC -fsanitize=address -fsanitize=undefined)
endif()
