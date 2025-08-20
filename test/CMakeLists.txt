include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

include(../external/bell/external/doctest/scripts/cmake/doctest.cmake)

add_executable(cspot-test
  main.cpp CredentialsResolverTest.cpp
)
target_compile_options(cspot-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(cspot-test cspot doctest::doctest trompeloeil::trompeloeil)

doctest_discover_tests(cspot-test)
