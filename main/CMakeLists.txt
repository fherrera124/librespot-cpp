cmake_minimum_required(VERSION 3.10)

project(cspot)

# Configurable options
option(CSPOT_DISABLE_TESTS "Disable cspot unit tests" ON)
set(CSPOT_EXTERNAL_BELL "" CACHE STRING "External bell library target name, optional")

# CMake options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")

if(MSVC)
    add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142 /wd4996)
endif()

# Main library sources
file(GLOB SOURCES "src/*.cpp" "src/events/*.cpp" "src/*.c" "src/api/*.cpp" "src/crypto/*.cpp" "src/audio/*.cpp" "src/proto/*.cpp")

# Enable testing
if(NOT CSPOT_DISABLE_TESTS)
    enable_testing()
    add_subdirectory("../test" ${CMAKE_CURRENT_BINARY_DIR}/test)
endif()

# Use externally specified bell library or the submodule
if(CSPOT_EXTERNAL_BELL)
    list(APPEND EXTRA_LIBS ${CSPOT_EXTERNAL_BELL})
else()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/bell ${CMAKE_CURRENT_BINARY_DIR}/bell)
    list(APPEND EXTRA_LIBS bell)
endif()

# Include websocketpp
add_subdirectory(../external/websocketpp ${CMAKE_CURRENT_BINARY_DIR}/websocketpp)

# Header only lib, create an interface for it
add_library(websockets INTERFACE)
target_include_directories(websockets INTERFACE ${websocketpp_SOURCE_DIR})
target_compile_options(websockets INTERFACE)

list(APPEND EXTRA_LIBS websockets)

# Include picoJSON
add_subdirectory(../external/picojson ${CMAKE_CURRENT_BINARY_DIR}/picojson)
list(APPEND EXTRA_LIBS picojson)

# Include yajl
add_subdirectory(../external/yajl ${CMAKE_CURRENT_BINARY_DIR}/yajl)
list(APPEND EXTRA_LIBS yajl)

# Add nanopb module
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/../external/nanopb/extra)
find_package(Nanopb REQUIRED)

# Build protobuf code
file(GLOB PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/../protobuf/*.proto)
nanopb_generate_cpp(TARGET cspot-proto ${PROTOS})
list(APPEND EXTRA_LIBS cspot-proto)
target_compile_definitions(cspot-proto PUBLIC PB_ENABLE_MALLOC)
target_compile_options(websockets INTERFACE -Wno-null-pointer-subtraction)

# target_compile_definitions(cspot-proto PUBLIC PB_FIELD_32BIT)
add_library(cspot STATIC ${SOURCES} ${PROTO_SRCS})

target_link_libraries(cspot PUBLIC ${EXTRA_LIBS})
target_include_directories(cspot PUBLIC "include" ${CMAKE_CURRENT_BINARY_DIR} ${EXTRA_INCLUDES})
