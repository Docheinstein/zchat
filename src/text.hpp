#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zchat::text {

// Makes untrusted text safe to print: drops control characters (which could inject terminal escape sequences),
// replaces invalid UTF-8 with '?', and truncates to at most max_bytes without splitting a code point.
std::string sanitize(std::string_view in, std::size_t max_bytes);

// Appends the UTF-8 encoding of a code point to out.
void append(std::string& out, char32_t cp);

} // namespace zchat::text
