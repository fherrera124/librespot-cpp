project(cspot_cli)
cmake_minimum_required(VERSION 3.18)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../main ${CMAKE_CURRENT_BINARY_DIR}/cspot)

file(GLOB SOURCES "*.cpp")
include_directories(".")

find_package(ALSA REQUIRED)

add_executable(cspot_cli ${SOURCES})
target_include_directories(cspot_cli PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(cspot_cli cspot ${CMAKE_DL_LIBS} ${ALSA_LIBRARIES})
