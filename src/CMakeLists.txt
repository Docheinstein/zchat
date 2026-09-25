add_executable(zchat)

target_sources(zchat PUBLIC
    chat.cpp
    main.cpp
    names.cpp
    net.cpp
    protocol.cpp
    terminal.cpp
    text.cpp
)

target_include_directories(zchat PUBLIC .)

find_package(Threads REQUIRED)
target_link_libraries(zchat PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(zchat PRIVATE /W4)
else()
    target_compile_options(zchat PRIVATE -Wall -Wextra)
endif()

if (WIN32)
    target_compile_definitions(zchat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_link_libraries(zchat PRIVATE ws2_32 iphlpapi shell32)
endif()
if (MSVC)
    target_compile_options(zchat PRIVATE /utf-8)
endif()
if (MINGW)
    # A self-contained .exe, without the MinGW runtime DLLs next to it.
    target_link_options(zchat PRIVATE -static)
endif()

install(TARGETS zchat)
