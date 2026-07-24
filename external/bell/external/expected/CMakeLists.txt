cmake_minimum_required(VERSION 3.14)

# Note: Stripped the original CMakeLists.txt to reduce it to just the header include
add_library(expected INTERFACE)
target_include_directories(expected INTERFACE include)
