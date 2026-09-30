# Turns a file into a C++ header holding its bytes, to build it into the executable (like the page of the window).
#
#   cmake -DINPUT=file -DOUTPUT=header.hpp -DNAME=symbol -P embed.cmake
#
# The header defines `NAME` (the bytes) and `NAME_size`. Bytes, not a string literal: MSVC limits those to 16 KB.

file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
# A line break every 32 bytes, for a header that tools do not choke on.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n    " bytes "${bytes}")

set(header "// Made by cmake/embed.cmake from ${INPUT}: do not edit.\n#pragma once\n\n#include <cstddef>\n\n")
string(APPEND header "inline constexpr unsigned char ${NAME}[] = {\n    ${bytes}\n};\n")
string(APPEND header "inline constexpr std::size_t ${NAME}_size = ${size};\n")

# Only rewritten when it changes, so what includes it is not rebuilt for nothing.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
    if(old STREQUAL header)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${header}")
