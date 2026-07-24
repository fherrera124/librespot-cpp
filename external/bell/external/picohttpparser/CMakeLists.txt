cmake_minimum_required(VERSION 3.16)
project(picohttpparser)

# Add the library target
add_library(picohttpparser STATIC picohttpparser.c)
target_include_directories(picohttpparser PUBLIC ".")