add_executable(zchat)

target_sources(zchat PUBLIC
    channel.cpp
    chat.cpp
    cipher.cpp
    color.cpp
    config.cpp
    dice.cpp
    file.cpp
    game.cpp
    history.cpp
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
    wordle.cpp
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

# The words /game wordle accepts as guesses, built into the executable.
set(zchat_words "${PROJECT_SOURCE_DIR}/third_party/enable/words5.txt")
add_custom_command(
    OUTPUT "${zchat_generated}/wordle_words.hpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${zchat_words}" "-DOUTPUT=${zchat_generated}/wordle_words.hpp"
            -DNAME=wordle_words -P "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${zchat_words}" "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
    VERBATIM)
target_sources(zchat PRIVATE "${zchat_generated}/wordle_words.hpp")
target_compile_definitions(zchat PRIVATE
    ZCHAT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ZCHAT_BUILD_DIR="${CMAKE_BINARY_DIR}"
    ZCHAT_BUILD_CONFIG="$<CONFIG>"
    ZCHAT_BUILT_EXE="$<TARGET_FILE:zchat>"
    ZCHAT_CMAKE="${CMAKE_COMMAND}"
    ZCHAT_GIT="${zchat_git}")

find_package(Threads REQUIRED)
target_link_libraries(zchat PRIVATE Threads::Threads)

# The window (see gui.hpp): the system's web view, through the webview library, which CMake downloads. On Windows
# and macOS it needs nothing more; on Linux, GTK and WebKitGTK (e.g. sudo apt install libgtk-3-dev
# libwebkit2gtk-4.1-dev). Without them, zchat is built for the terminal only.
option(ZCHAT_GUI "Build the zchat window (zchat --terminal still works without it)" ON)
if (ZCHAT_GUI AND UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(zchat_webkit QUIET webkit2gtk-4.1)
        if (NOT zchat_webkit_FOUND)
            pkg_check_modules(zchat_webkit QUIET webkit2gtk-4.0)
        endif()
    endif()
    if (NOT zchat_webkit_FOUND)
        message(WARNING "WebKitGTK was not found: zchat is built for the terminal only. For the window, install "
                        "GTK and WebKitGTK (e.g. sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev) and run "
                        "cmake again.")
        set(ZCHAT_GUI OFF)
    endif()
endif()
if (ZCHAT_GUI)
    include(FetchContent)
    set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(webview
        GIT_REPOSITORY https://github.com/webview/webview
        GIT_TAG 0.12.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(webview)

    # The page of the window, built into the executable.
    add_custom_command(
        OUTPUT "${zchat_generated}/ui_html.hpp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
                "-DOUTPUT=${zchat_generated}/ui_html.hpp" -DNAME=ui_html -P "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html" "${PROJECT_SOURCE_DIR}/cmake/embed.cmake"
        VERBATIM)
    target_sources(zchat PRIVATE gui.cpp "${zchat_generated}/ui_html.hpp")
    target_link_libraries(zchat PRIVATE webview::core_static)
    target_compile_definitions(zchat PRIVATE ZCHAT_GUI=1)
endif()

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
