cmake_minimum_required(VERSION 3.10)

project(Tremor C)

# find_package(Ogg REQUIRED)

# Define the source files for the libvorbisidec library
set(LIBVORBISIDEC_SOURCES
    mdct.c
    block.c
    window.c
    synthesis.c
    info.c
    floor1.c
    floor0.c
    vorbisfile.c
    res012.c
    mapping0.c
    registry.c
    codebook.c
    sharedbook.c
)

add_library(vorbisidec STATIC ${LIBVORBISIDEC_SOURCES})

target_link_libraries(vorbisidec PUBLIC Ogg::ogg)
target_compile_options(vorbisidec PRIVATE -Wno-incompatible-pointer-types)
target_include_directories(vorbisidec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(vorbisidec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
