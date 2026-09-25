#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zchat::text {

// Makes untrusted text safe to print: drops control characters (which could inject terminal escape sequences),
// replaces invalid UTF-8 with '?', and truncates to at most max_bytes without splitting a code point.
std::string sanitize(std::string_view in, std::size_t max_bytes);

// The trailing part of s that holds at most max_chars code points.
std::string_view tail(std::string_view s, std::size_t max_chars);

// Removes the last code point of s, if any.
void pop_back(std::string& s);

// Appends the UTF-8 encoding of a code point to out.
void append(std::string& out, char32_t cp);

} // namespace zchat::text
