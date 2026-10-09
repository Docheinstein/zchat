# zchat_tests: the tests of what zchat can test without a network nor a window. Every .cpp here is a part of it, with
# the sources of zchat it tests; `just test` (or ctest) builds and runs them.
file(GLOB zchat_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(zchat_tests
    ${zchat_test_sources}
    "${PROJECT_SOURCE_DIR}/src/casino.cpp"
    "${PROJECT_SOURCE_DIR}/src/channel.cpp"
    "${PROJECT_SOURCE_DIR}/src/coins.cpp"
    "${PROJECT_SOURCE_DIR}/src/color.cpp"
    "${PROJECT_SOURCE_DIR}/src/elo.cpp"
    "${PROJECT_SOURCE_DIR}/src/markup.cpp"
    "${PROJECT_SOURCE_DIR}/src/pokemon_state.cpp"
    "${PROJECT_SOURCE_DIR}/src/process.cpp"
    "${PROJECT_SOURCE_DIR}/src/text.cpp"
    "${PROJECT_SOURCE_DIR}/src/world_place.cpp"
)
find_package(Threads REQUIRED)
target_link_libraries(zchat_tests PRIVATE Threads::Threads)

target_include_directories(zchat_tests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
# The sample battles the tests replay.
target_compile_definitions(zchat_tests PRIVATE ZCHAT_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/pokemon")

if (MSVC)
    target_compile_options(zchat_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(zchat_tests PRIVATE -Wall -Wextra)
endif()
if (WIN32)
    target_compile_definitions(zchat_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif()

add_test(NAME zchat_tests COMMAND zchat_tests)
