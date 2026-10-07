#pragma once

#include <optional>
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
    // A short name that works the same, e.g. "b" for "bold"; empty when there is none.
    std::string_view alias = {};
};

// All the tags, for /tags.
std::span<const Tag> tags();

// The tag with this name or alias (case-insensitive), or nullptr.
const Tag* find_tag(std::string_view name);

// Turns a (sanitized) message into what to print: tags are removed and, when colors is true, applied as ANSI
// escape sequences. Anything that is not a valid tag is shown as it is.
std::string render(std::string_view text, bool colors);

// A message quoting another one, as it is sent: "<quote=NAME>TEXT</quote>REPLY". Versions without quotes show it
// as it is.
struct Quote {
    std::string_view name;
    std::string_view text;
    std::string_view reply;
};

// The quote a message starts with, if it does, and the reply after it (without the spaces before it).
std::optional<Quote> split_quote(std::string_view message);

// What to put before a reply to quote a message said by name: the message (its reply, when it quotes another one)
// without its tags, shortened when it is long, and a space.
std::string quote(std::string_view name, std::string_view message);

} // namespace zchat::markup
