idf_component_register(INCLUDE_DIRS "."
  REQUIRES driver)


# Don't use the self-contained mbedtls build
set(BELL_EXTERNAL_MBEDTLS "mbedtls" CACHE STRING "External mbedtls library target name, optional") 
set(BELL_DISABLE_MDNS ON CACHE BOOL "Don't include MDNS (Bonjour, Avahi) wrappers")

add_subdirectory(../../../../ ${CMAKE_CURRENT_BINARY_DIR}/bell)
target_link_libraries(${COMPONENT_LIB} INTERFACE bell)