#pragma once

#include <span>
#include <string>
#include <string_view>

namespace zchat::markup {

// A tag that can be used in messages, like an HTML tag: "<name=value>text</name>". The tags themselves are not
// shown; they affect the text after them until they are closed, or until the end of the message.
struct Tag {
    std::string_view name;
    std::string_view summary;
    std::string_view explanation;
    std::string_view example;
};

// All the tags, for /tags.
std::span<const Tag> tags();

// The tag with this name (case-insensitive), or nullptr.
const Tag* find_tag(std::string_view name);

// Turns a (sanitized) message into what to print: tags are removed and, when colors is true, applied as ANSI
// escape sequences. Anything that is not a valid tag is shown as it is.
std::string render(std::string_view text, bool colors);

} // namespace zchat::markup
