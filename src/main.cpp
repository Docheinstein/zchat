#include "chat.hpp"
#include "names.hpp"
#include "net.hpp"
#include "terminal.hpp"
#include "text.hpp"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::string_view version = "0.1.0";
constexpr std::uint16_t default_port = 47474;

struct Options {
    std::uint16_t port = default_port;
    std::optional<std::string> name;
};

void print_usage() {
    std::printf("Usage: zchat [options]\n"
                "\n"
                "Chat with everyone running zchat on your local network.\n"
                "You get a random cool name, then just type and press Enter.\n"
                "\n"
                "Options:\n"
                "  -p, --port PORT   UDP port shared by the chat room (default %u)\n"
                "  -n, --name NAME   use NAME instead of a random one\n"
                "  -h, --help        show this help\n"
                "  -v, --version     show the version\n",
                static_cast<unsigned>(default_port));
}

// Returns nullopt, after printing why, when the program should exit instead of chatting.
std::optional<Options> parse_args(int argc, char** argv, int& exit_code) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "zchat: %s needs a value\n", argv[i]);
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };
        if (arg == "-h" || arg == "--help") {
            print_usage();
            exit_code = 0;
            return std::nullopt;
        }
        if (arg == "-v" || arg == "--version") {
            std::printf("zchat %.*s\n", static_cast<int>(version.size()), version.data());
            exit_code = 0;
            return std::nullopt;
        }
        if (arg == "-p" || arg == "--port") {
            const auto v = value();
            unsigned port = 0;
            if (!v || std::from_chars(v->data(), v->data() + v->size(), port).ptr != v->data() + v->size() ||
                port == 0 || port > 65535) {
                if (v) {
                    std::fprintf(stderr, "zchat: invalid port '%.*s'\n", static_cast<int>(v->size()), v->data());
                }
                exit_code = 2;
                return std::nullopt;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (arg == "-n" || arg == "--name") {
            const auto v = value();
            const std::string name = v ? zchat::text::sanitize(*v, zchat::max_name_bytes) : std::string();
            if (name.empty() || name.find_first_not_of(' ') == std::string::npos) {
                if (v) {
                    std::fprintf(stderr, "zchat: invalid name\n");
                }
                exit_code = 2;
                return std::nullopt;
            }
            options.name = name;
        } else {
            std::fprintf(stderr, "zchat: unknown option '%s' (try --help)\n", argv[i]);
            exit_code = 2;
            return std::nullopt;
        }
    }
    return options;
}

#ifdef _WIN32
// Closing the console window kills the process: say goodbye first, so the others know right away.
std::atomic<zchat::Chat*> active_chat {nullptr};

BOOL WINAPI on_console_event(DWORD event) {
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (zchat::Chat* chat = active_chat.load()) {
            chat->stop();
        }
    }
    return FALSE;
}
#endif

void print_help(zchat::Chat& chat) {
    chat.notice("Commands:");
    chat.notice("  /who    list the people in the chat");
    chat.notice("  /help   show this help");
    chat.notice("  /quit   leave the chat (or Ctrl+C, Ctrl+D)");
}

void print_who(zchat::Chat& chat) {
    const auto peers = chat.peers();
    if (peers.empty()) {
        chat.notice("Nobody else is here yet.");
        return;
    }
    std::string list;
    for (const auto& peer : peers) {
        list += list.empty() ? "" : ", ";
        list += peer;
    }
    chat.notice(std::format("{} other{} here: {}", peers.size(), peers.size() == 1 ? " is" : "s are", list));
}

int run(const Options& options) {
    zchat::net::NetworkInit network;

    std::string name;
    if (options.name) {
        name = *options.name;
    } else {
        std::mt19937_64 rng(std::random_device {}());
        name = zchat::random_name(rng);
    }

    zchat::Terminal terminal;
    zchat::Chat chat(options.port, name, terminal);
#ifdef _WIN32
    active_chat = &chat;
    SetConsoleCtrlHandler(on_console_event, TRUE);
#endif

    chat.notice(std::format("Welcome to zchat! You are {}.", chat.colored_own_name()));
    chat.notice(
        std::format("Chatting on UDP port {}. Type a message and press Enter, /help for commands.", options.port));
    terminal.set_prompt(terminal.colors() ? "\x1b[1m>\x1b[0m " : "> ", 2);
    chat.start();

    while (auto line = terminal.read_line()) {
        std::string_view input = *line;
        while (!input.empty() && input.back() == ' ') {
            input.remove_suffix(1);
        }
        while (!input.empty() && input.front() == ' ') {
            input.remove_prefix(1);
        }
        if (input.empty()) {
            continue;
        }
        if (input == "/quit" || input == "/exit" || input == "/q") {
            break;
        }
        if (input == "/who" || input == "/list") {
            print_who(chat);
        } else if (input == "/help" || input == "/?") {
            print_help(chat);
        } else if (input.starts_with('/') && !input.starts_with("//")) {
            chat.notice(std::format("Unknown command {} (try /help)", zchat::text::sanitize(input, 64)));
        } else {
            // "//text" sends a message starting with a slash.
            chat.say(input.starts_with("//") ? input.substr(1) : input);
        }
    }

    chat.stop();
#ifdef _WIN32
    active_chat = nullptr;
#endif
    chat.notice("Bye!");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int exit_code = 0;
    const auto options = parse_args(argc, argv, exit_code);
    if (!options) {
        return exit_code;
    }
    try {
        return run(*options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "zchat: %s\n", e.what());
        return 1;
    }
}
