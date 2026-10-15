#include "commands.hpp"

#include <array>
#include <cctype>

namespace zchat::commands {

namespace {

    // As print_help() in main.cpp tells about them: a command added there goes here too.
    constexpr std::array<Command, 39> table {{
        {"/add", "[#CHANNEL] NAME", "add someone to a channel"},
        {"/addemoji", "NAME [SIZE] FILE", "save a picture as an emoji"},
        {"/ascii", "[SIZE] [TEXTURE%] FILE", "send a picture drawn with characters"},
        {"/avatar", "FILE", "set your avatar (/avatar none removes it)"},
        {"/casino", "[help]", "bet your coins at blackjack, roulette and the horses"},
        {"/changelog", "[N|all]", "what is new in zchat"},
        {"/channels", "", "list the channels"},
        {"/coins", "[NAME|top]", "your coins, or somebody's"},
        {"/color", "NAME", "change the color of your name"},
        {"/create", "NAME [public|private]", "make a channel"},
        {"/delete", "NAME", "delete a channel of yours"},
        {"/dnd", "[30m|1h|8h|off]", "Do Not Disturb"},
        {"/emoji", "[NAME] [ascii]", "send a saved emoji (alone: list yours)"},
        {"/file", "FILE", "send a file, for everyone to download"},
        {"/forget", "", "forget the saved name and get a new one"},
        {"/game", "[help NAME]", "the games everyone can play"},
        {"/help", "[game]", "show the help"},
        {"/image", "[SIZE] FILE", "send a picture"},
        {"/join", "NAME", "go to a channel"},
        {"/kick", "NAME", "ask the others to vote NAME out (/kick yes, /kick no)"},
        {"/leave", "[NAME]", "leave a channel or a private chat"},
        {"/members", "[#CHANNEL]", "who is in a channel"},
        {"/msg", "NAME [MESSAGE]", "a private chat with NAME"},
        {"/nick", "NAME", "change your name"},
        {"/notify", "[SETTING]", "the notification settings"},
        {"/quit", "", "leave the chat"},
        {"/quote", "[@NAME] MESSAGE", "reply quoting the last message"},
        {"/remove", "[#CHANNEL] NAME", "remove someone from a channel"},
        {"/removeemoji", "NAME", "delete a saved emoji"},
        {"/save", "[N]", "save file N in your downloads (alone: list them)"},
        {"/shop", "", "the prices of /trill, /kick and /spy"},
        {"/spy", "@NAME", "ask to see NAME's screen (/spy allow, /spy deny)"},
        {"/stop", "", "stop the trills coming at you"},
        {"/tags", "[NAME]", "the tags for messages, like <color=red>"},
        {"/trill", "[SOUND] [PICTURE]", "trill everybody else"},
        {"/update", "", "get the latest zchat and restart"},
        {"/who", "", "list the people in the chat"},
        {"/whoami", "", "show your name"},
        {"/world", "[who]", "enter the world, with everybody"},
    }};

    char lower(char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

} // namespace

std::span<const Command> all() {
    return table;
}

bool runs_alone(const Command& command) {
    // Anything outside the brackets is required.
    int depth = 0;
    for (const char c : command.args) {
        if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        } else if (depth == 0 && c != ' ') {
            return false;
        }
    }
    return true;
}

std::vector<Command> matching(std::string_view typed) {
    std::vector<Command> found;
    if (!typed.starts_with('/') || typed.starts_with("//") || typed.find(' ') != std::string_view::npos) {
        return found;
    }
    for (const auto& command : table) {
        bool starts = command.name.size() >= typed.size();
        for (std::size_t i = 0; starts && i < typed.size(); ++i) {
            starts = lower(typed[i]) == command.name[i];
        }
        if (starts) {
            found.push_back(command);
        }
    }
    return found;
}

} // namespace zchat::commands
