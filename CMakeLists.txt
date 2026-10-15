cmake_minimum_required(VERSION 3.16)
project(zchat CXX)

set(CMAKE_CXX_STANDARD 23)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(src)

# The 3D hub (hub/): an experiment, which /hub starts. Windows only for now.
if (WIN32)
    option(ZCHAT_HUB "Build the 3D hub (zchat_hub), which /hub starts" ON)
else()
    set(ZCHAT_HUB OFF)
endif()
if (ZCHAT_HUB)
    add_subdirectory(hub)
    # Built with zchat (and so by /update too), and told about.
    add_dependencies(zchat zchat_hub)
    target_compile_definitions(zchat PRIVATE ZCHAT_HUB=1)
endif()

# The tests (see tests/check.hpp): `just test`, or ctest after building zchat_tests.
option(ZCHAT_TESTS "Build the tests" ON)
if (ZCHAT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
