cmake_minimum_required(VERSION 3.3)

project(cspotcli)

set(CMAKE_BUILD_TYPE Debug CACHE STRING "CMake Build Type")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# CMake options
set(CMAKE_CXX_STANDARD 20)

if(MSVC)
	add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
	add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142)
endif()

# Main target sources
file(GLOB SOURCES "*.cpp")

find_package(portaudio REQUIRED )
include_directories( ${PORTAUDIO_INCLUDE_DIRS} )
set(THINGS_TO_LINK ${THINGS_TO_LINK} ${PORTAUDIO_LIBRARIES})

# BELL_SINK_ALSA/BELL_SINK_PORTAUDIO are bell/CMakeLists.txt's own options
# (see external/bell/CMakeLists.txt) - they decide whether
# ALSAAudioSink.cpp/PortAudioSink.cpp are even compiled into the bell
# target, and are already resolved by the time this file runs (bell is
# add_subdirectory()'d from the root CMakeLists.txt before extras/cli).
# Setting them here would be too late to have any effect - pass
# -DBELL_SINK_ALSA=ON (or -DBELL_SINK_PORTAUDIO=ON) on the cmake command
# line instead. This just picks which sink cspotcli's own main.cpp uses.
if(BELL_SINK_ALSA)
    add_definitions(-DCSPOT_ENABLE_ALSA_SINK)
elseif(BELL_SINK_PORTAUDIO)
    add_definitions(-DCSPOT_ENABLE_PORTAUDIO_SINK)
endif()

# The cspot and bell library targets are already defined by the root CMakeLists.txt by the time
# it descends into this directory (add_subdirectory(extras/cli)) - no
# need to reach for them here anymore.

# Configure the target
add_executable(cspotcli ${SOURCES})
target_link_libraries(cspotcli PUBLIC cspot ${EXTRA_LIBS}  ${THINGS_TO_LINK})
add_compile_options(-fsanitize=address -fno-omit-frame-pointer -O1)
add_link_options(-fsanitize=address)
target_include_directories(cspotcli PRIVATE "." ${EXTRA_INCLUDES})