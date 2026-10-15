#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace zchat::commands {

// A command that can be typed, as the list of suggestions shows it while it is typed (see matching()): its name with
// the '/', what it takes after it (as /help writes it: [OPTIONAL], REQUIRED), and what it does, in a few words.
struct Command {
    std::string_view name;
    std::string_view args;
    std::string_view about;
};

// The commands /help tells about (without their other names, like /dm for /msg), in alphabetical order.
std::span<const Command> all();

// Whether the command does something typed alone, without what it takes: nothing, or only [OPTIONAL] things. Then
// Enter on the suggestion runs it, else it is completed for the rest to be typed.
bool runs_alone(const Command& command);

// The commands starting with what was typed of a command (the '/' and some letters, no spaces), ignoring case; none
// for anything else, like "//" which starts a message with a slash.
std::vector<Command> matching(std::string_view typed);

} // namespace zchat::commands
