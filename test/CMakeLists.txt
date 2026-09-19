include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

include(../external/bell/external/doctest/scripts/cmake/doctest.cmake)

# Read the directory for the test data
set(TEST_DATA_PATH "${CMAKE_CURRENT_SOURCE_DIR}/data")

add_executable(cspot-test
  main.cpp CredentialsResolverTest.cpp SpClientTest.cpp TrackQueueHandlerTest.cpp StreamPlayerTest.cpp SessionStoreTest.cpp LoudnessNormalisationTest.cpp
)
target_compile_options(cspot-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_compile_definitions(cspot-test PRIVATE TEST_DATA_PATH="${TEST_DATA_PATH}")
target_link_libraries(cspot-test cspot doctest::doctest trompeloeil::trompeloeil)

doctest_discover_tests(cspot-test)

# Protocol regressions also run independently of the legacy mock-based suite.
add_executable(cspot-protocol-test ProtocolRegressionTest.cpp)
target_compile_definitions(cspot-protocol-test PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_options(cspot-protocol-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(cspot-protocol-test PRIVATE cspot doctest::doctest)
doctest_discover_tests(cspot-protocol-test)

add_executable(cspot-audio-task-test AudioMemoryTaskRegressionTest.cpp)
target_compile_definitions(cspot-audio-task-test PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_options(cspot-audio-task-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(cspot-audio-task-test PRIVATE cspot doctest::doctest)
doctest_discover_tests(cspot-audio-task-test PROPERTIES TIMEOUT 30)
