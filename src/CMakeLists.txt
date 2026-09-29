add_executable(zchat)

target_sources(zchat PUBLIC
    chat.cpp
    cipher.cpp
    color.cpp
    config.cpp
    dice.cpp
    game.cpp
    image.cpp
    main.cpp
    markup.cpp
    names.cpp
    net.cpp
    paint.cpp
    process.cpp
    race.cpp
    protocol.cpp
    terminal.cpp
    text.cpp
    update.cpp
)

target_include_directories(zchat PUBLIC .)
# Third party code: its warnings are not ours to fix.
target_include_directories(zchat SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)

# /update rebuilds zchat from these sources, in this build folder, and needs to know which commit it runs.
find_package(Git QUIET)
set(zchat_git git)
if (GIT_EXECUTABLE)
    set(zchat_git "${GIT_EXECUTABLE}")
endif()
set(zchat_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(zchat_commit
    COMMAND "${CMAKE_COMMAND}" "-DGIT=${zchat_git}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DOUTPUT=${zchat_generated}/commit.hpp" -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake"
    BYPRODUCTS "${zchat_generated}/commit.hpp"
    VERBATIM)
add_dependencies(zchat zchat_commit)
target_include_directories(zchat PRIVATE "${zchat_generated}")
target_compile_definitions(zchat PRIVATE
    ZCHAT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ZCHAT_BUILD_DIR="${CMAKE_BINARY_DIR}"
    ZCHAT_BUILD_CONFIG="$<CONFIG>"
    ZCHAT_BUILT_EXE="$<TARGET_FILE:zchat>"
    ZCHAT_CMAKE="${CMAKE_COMMAND}"
    ZCHAT_GIT="${zchat_git}")

find_package(Threads REQUIRED)
target_link_libraries(zchat PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(zchat PRIVATE /W4)
else()
    target_compile_options(zchat PRIVATE -Wall -Wextra)
endif()

if (WIN32)
    target_compile_definitions(zchat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_link_libraries(zchat PRIVATE ws2_32 iphlpapi shell32 ole32)
endif()
if (MSVC)
    target_compile_options(zchat PRIVATE /utf-8)
endif()
if (MINGW)
    # A self-contained .exe, without the MinGW runtime DLLs next to it.
    target_link_options(zchat PRIVATE -static)
endif()

install(TARGETS zchat)
