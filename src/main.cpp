#include "chat.hpp"
#include "config.hpp"
#include "game.hpp"
#ifdef ZCHAT_GUI
#include "gui.hpp"
#endif
#include "history.hpp"
#include "image.hpp"
#include "markup.hpp"
#include "names.hpp"
#include "net.hpp"
#include "terminal.hpp"
#include "text.hpp"
#include "update.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::string_view version = "0.1.0";
constexpr std::uint16_t default_port = 47474;

struct Options {
    std::uint16_t port = default_port;
    std::optional<std::string> name;
    // In the terminal instead of a window.
    bool terminal = false;
};

void print_usage() {
    std::printf("Usage: zchat [options]\n"
                "\n"
                "Chat with everyone running zchat on your local network.\n"
                "You get a random cool name, then just type and press Enter.\n"
                "\n"
                "Options:\n"
                "  -p, --port PORT   UDP port shared by the chat room (default %u)\n"
                "  -n, --name NAME   use NAME for this session, instead of the saved or a random one\n"
                "  -t, --terminal    chat in the terminal instead of a window\n"
                "  -h, --help        show this help\n"
                "  -v, --version     show the version\n",
                static_cast<unsigned>(default_port));
}

// Makes a name typed by the user (or read from the config) usable: sanitized and trimmed; empty when invalid.
std::string clean_name(std::string_view raw) {
    std::string name = zchat::text::sanitize(raw, zchat::max_name_bytes);
    const auto first = name.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return name.substr(first, name.find_last_not_of(' ') - first + 1);
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
            const std::string name = v ? clean_name(*v) : std::string();
            if (name.empty()) {
                if (v) {
                    std::fprintf(stderr, "zchat: invalid name\n");
                }
                exit_code = 2;
                return std::nullopt;
            }
            options.name = name;
        } else if (arg == "-t" || arg == "--terminal") {
            options.terminal = true;
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
    chat.notice("  /who         list the people in the chat");
    chat.notice("  /whoami      show your name");
    chat.notice("  /nick NAME   change your name, and keep it for next time");
    chat.notice("  /forget      forget the saved name and get a new random one");
    chat.notice("  /color NAME  change the color of your name, and keep it for next time");
    chat.notice("               (a name from /color, #rrggbb, or r,g,b)");
    chat.notice("  /color random  pick a random color (not kept)");
    chat.notice("  /tags        list the tags for messages, like <color=red>text</color>");
    chat.notice("  /tags NAME   explain a tag, with an example");
    chat.notice("  /image [SIZE] FILE  send a picture (windows show it, terminals draw it with characters)");
    chat.notice("  /ascii [SIZE] [TEXTURE%] FILE  send a picture drawn with characters, for everyone");
    chat.notice("               (SIZE: small, medium, large, a width like 40, or 40x20;");
    chat.notice("                TEXTURE%: 0% blocks only, the default, to 100% symbols only)");
    chat.notice("  /addemoji NAME [SIZE] [TEXTURE%] FILE  save a picture as an emoji");
    chat.notice("  /emoji NAME  send a saved emoji (/emoji alone lists yours)");
    chat.notice("  /removeemoji NAME  delete a saved emoji");
    chat.notice("               (or drop an image file on the window, then press Enter)");
    chat.notice("  @NAME        tag someone in a message: they hear a sound");
    chat.notice("               (type @ to pick from the list with Up/Down, then Enter or Tab)");
    chat.notice("  @everyone    tag all the people in the chat: they all hear a sound");
    chat.notice("  /game        list the games everyone in the chat can play");
    chat.notice("  /game NAME   start one, like /game race: the first to type the words shown wins");
    chat.notice("  /game dice roll, /game dice stop  roll the die or keep your points, in a round of dice");
    chat.notice("  /game paint c7 red  color a square of the shared canvas (/game paint shows it)");
    chat.notice("  /game paint color red  pick your brush, for /game paint c7 without a color");
    chat.notice("  /update      get the latest zchat, build it and restart");
    chat.notice("  /help        show this help");
    chat.notice("  /quit        leave the chat (or Ctrl+C, Ctrl+D)");
}

void change_nick(zchat::Chat& chat, std::string_view arg) {
    if (arg.empty()) {
        chat.notice(std::format("You are {}. Use /nick NAME to change it.", chat.colored_own_name()));
        return;
    }
    const std::string name = clean_name(arg);
    if (name.empty()) {
        chat.notice("That name is not valid.");
        return;
    }
    chat.set_name(name);
    chat.notice(std::format("You are now known as {}", chat.colored_own_name()));
    if (!zchat::config::set("name", name)) {
        chat.notice(std::format("Could not save your name to {}", zchat::config::file().string()));
    }
}

void forget_nick(zchat::Chat& chat, std::mt19937_64& rng) {
    if (!zchat::config::remove("name")) {
        chat.notice(std::format("Could not forget your name in {}", zchat::config::file().string()));
        return;
    }
    // A new name, not the same one again by chance.
    const std::string old_name = chat.name();
    std::string name;
    do {
        name = zchat::random_name(rng);
    } while (name == old_name);
    chat.set_name(name);
    chat.notice(std::format("Saved name forgotten. You are now known as {}", chat.colored_own_name()));
}

void change_color(zchat::Chat& chat, std::string_view arg, std::mt19937_64& rng) {
    if (arg.empty()) {
        std::string list;
        for (std::size_t i = 0; i < zchat::palette_size(); ++i) {
            list += list.empty() ? "" : " ";
            list += chat.paint(zchat::palette_color(i), zchat::palette_name(i));
        }
        chat.notice(std::format("Your color is {}. Use /color NAME to change it, or /color random.",
                                chat.paint(chat.color(), zchat::describe(chat.color()))));
        chat.notice(std::format("Colors: {}", list));
        chat.notice("Or any color as a hex code or RGB values: /color #ff8800, /color 255,136,0");
        return;
    }
    zchat::Color color;
    const bool random = arg == "random";
    if (random) {
        // A palette color different from the current one, so the command always does something.
        do {
            color = zchat::palette_color(std::uniform_int_distribution<std::size_t>(0, zchat::palette_size() - 1)(rng));
        } while (color == chat.color());
    } else if (const auto parsed = zchat::parse_color(arg)) {
        color = *parsed;
    } else {
        chat.notice(std::format("Unknown color {}: use a name from /color, a hex code like #ff8800, or RGB values "
                                "like 255,136,0",
                                zchat::text::sanitize(arg, 32)));
        return;
    }
    chat.set_color(color);
    chat.notice(std::format("Your name is now {}", chat.colored_own_name()));
    // A random color is not kept, like the random color zchat starts with when none is saved.
    const bool saved = random ? zchat::config::remove("color") : zchat::config::set("color", zchat::describe(color));
    if (!saved) {
        chat.notice(std::format("Could not save your color to {}", zchat::config::file().string()));
    }
}

void print_tags(zchat::Chat& chat, std::string_view arg, bool colors) {
    if (arg.empty()) {
        chat.notice("Tags you can use in messages, like HTML: <tag=value>text</tag>. /tags NAME tells more.");
        for (const auto& tag : zchat::markup::tags()) {
            const std::string names = tag.alias.empty() ? std::string(tag.name)
                                                        : std::format("{}, {}", tag.name, tag.alias);
            chat.notice(std::format("  {:<17} {}", names, tag.summary));
        }
        return;
    }
    const auto* tag = zchat::markup::find_tag(arg);
    if (!tag) {
        chat.notice(std::format("Unknown tag {} (try /tags)", zchat::text::sanitize(arg, 32)));
        return;
    }
    chat.notice(std::format("<{}>: {}", tag->name, tag->explanation));
    chat.notice(std::format("Example: {}", tag->example));
    chat.notice(std::format("Shows as: {}", zchat::markup::render(tag->example, colors)));
}

struct ArtSize {
    std::size_t cols = zchat::max_art_cols;
    std::size_t rows = zchat::max_art_rows;
};

constexpr std::size_t min_art_cols = 4;
constexpr std::size_t min_art_rows = 2;

// Reads the size of /image [SIZE] FILE: small, medium or large, a width in characters ("40"), or a width and a
// height ("40x20"). The picture keeps its proportions, so it fits in that size without filling it. Sizes are
// limited by what the others can receive.
std::optional<ArtSize> parse_art_size(std::string_view s) {
    const auto is = [&](std::string_view word) {
        return std::ranges::equal(s, word, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
    };
    if (is("small")) {
        return ArtSize {24, zchat::max_art_rows};
    }
    if (is("medium")) {
        return ArtSize {40, zchat::max_art_rows};
    }
    if (is("large")) {
        return ArtSize {};
    }
    const auto number = [](std::string_view digits, std::size_t min, std::size_t max) -> std::optional<std::size_t> {
        std::size_t value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (digits.empty() || ec != std::errc {} || ptr != digits.data() + digits.size()) {
            return std::nullopt;
        }
        return std::clamp(value, min, max);
    };
    const auto x = s.find_first_of("xX");
    const auto cols = number(s.substr(0, x), min_art_cols, zchat::max_art_cols);
    if (!cols) {
        return std::nullopt;
    }
    if (x == std::string_view::npos) {
        return ArtSize {*cols, zchat::max_art_rows};
    }
    const auto rows = number(s.substr(x + 1), min_art_rows, zchat::max_art_rows);
    if (!rows) {
        return std::nullopt;
    }
    return ArtSize {*cols, *rows};
}

// Reads a percentage from 0% to 100%, like the texture of /image.
std::optional<int> parse_percent(std::string_view s) {
    if (!s.ends_with('%')) {
        return std::nullopt;
    }
    s.remove_suffix(1);
    int value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || value < 0 || value > 100) {
        return std::nullopt;
    }
    return value;
}

// What "[SIZE] [TEXTURE%] FILE" says, for /image, /ascii and /addemoji.
struct ImageArgs {
    ArtSize size;
    bool sized = false;
    std::optional<int> texture;
    std::string_view file;
};

// Reads "[SIZE] [TEXTURE%] FILE", or says why it cannot.
std::optional<ImageArgs> parse_image_args(zchat::Chat& chat, std::string_view arg) {
    // A size and a texture first, in any order, when there is something after them: "/image 40" alone still sends
    // a file named 40.
    ImageArgs args;
    while (true) {
        const auto space = arg.find(' ');
        if (space == std::string_view::npos) {
            break;
        }
        std::string_view rest = arg.substr(space + 1);
        while (!rest.empty() && rest.front() == ' ') {
            rest.remove_prefix(1);
        }
        const std::string_view word = arg.substr(0, space);
        if (rest.empty()) {
            break;
        }
        if (const auto percent = parse_percent(word); percent && !args.texture) {
            args.texture = *percent;
        } else if (word.ends_with('%') && !percent) {
            chat.notice(std::format("The texture is from 0% to 100%, not {}.", zchat::text::sanitize(word, 20)));
            return std::nullopt;
        } else if (const auto parsed = parse_art_size(word); parsed && !args.sized) {
            args.size = *parsed;
            args.sized = true;
        } else {
            break;
        }
        arg = rest;
    }
    args.file = arg;
    return args;
}

// Turns "[SIZE] [TEXTURE%] FILE" into a drawing, or says why it cannot ("Cannot <what> FILE: ...").
std::optional<std::string> draw_image(zchat::Chat& chat, std::string_view arg, std::string_view what) {
    const auto args = parse_image_args(chat, arg);
    if (!args) {
        return std::nullopt;
    }
    auto art = zchat::image::to_ascii(zchat::image::parse_path(args->file), args->size.cols, args->size.rows,
                                      args->texture.value_or(0));
    if (!art) {
        chat.notice(std::format("Cannot {} {}: {}", what, zchat::text::sanitize(args->file, 200), art.error()));
        return std::nullopt;
    }
    return std::move(*art);
}

// How big a real picture of a size is: large (the default) is as big as a packet takes well, and a width in characters
// is about as wide as that drawing would be (8 pixels a character).
constexpr int picture_pixels_per_char = 8;
constexpr int max_picture_size = 480;

void ascii_help(zchat::Chat& chat, std::string_view command) {
    chat.notice(std::format("Use {} [SIZE] [TEXTURE%] FILE to send a picture drawn with characters.", command));
    chat.notice(std::format("SIZE is small, medium, large (the default), a width like 40, or a width and height "
                            "like 40x20, up to {}x{}.",
                            zchat::max_art_cols, zchat::max_art_rows));
    chat.notice("TEXTURE% is how much is drawn with symbols instead of blocks: 0% none (the default), about 15% the "
                "dark parts, 100% no blocks at all.");
}

void send_ascii(zchat::Chat& chat, std::string_view arg) {
    if (arg.empty()) {
        ascii_help(chat, "/ascii");
        return;
    }
    if (const auto art = draw_image(chat, arg, "send")) {
        chat.draw(*art);
    }
}

void send_image(zchat::Chat& chat, std::string_view arg) {
    if (arg.empty()) {
        chat.notice("Use /image [SIZE] FILE to send a picture, or drop an image file on the window and press Enter. "
                    "Windows show it as it is, terminals draw it with characters.");
        chat.notice("SIZE is small, medium, large (the default), or a width in characters like 40.");
        chat.notice("For a picture drawn with characters for everyone, use /ascii [SIZE] [TEXTURE%] FILE (or /image "
                    "with a TEXTURE%).");
        return;
    }
    const auto args = parse_image_args(chat, arg);
    if (!args) {
        return;
    }
    // A texture is for pictures drawn with characters.
    if (args->texture) {
        send_ascii(chat, arg);
        return;
    }
    const int size = std::min(max_picture_size, static_cast<int>(std::min(args->size.cols * picture_pixels_per_char,
                                                                          args->size.rows * 2 * picture_pixels_per_char)));
    bool still = false;
    const auto picture = zchat::image::encode_picture(zchat::image::parse_path(args->file), size, &still);
    if (!picture) {
        chat.notice(std::format("Cannot send {}: {}", zchat::text::sanitize(args->file, 200), picture.error()));
        return;
    }
    if (still) {
        chat.notice("That animation is too long to send whole: sending its first frame.");
    }
    chat.send_picture(*picture);
}

// Emoji: pictures saved under a name, already drawn, in the emoji folder of the config folder, one NAME.art file
// each (the drawing as it is sent), so /emoji NAME sends them without the image file.
constexpr std::size_t max_emoji_name = 32;
constexpr std::uintmax_t max_emoji_bytes = 64 * 1024;

std::filesystem::path emoji_dir() {
    const auto dir = zchat::config::dir();
    return dir.empty() ? dir : dir / "emoji";
}

// The name of an emoji, lowercase: letters, digits, '-' and '_'. Empty when it is not one.
std::string emoji_name(std::string_view s) {
    while (!s.empty() && s.front() == ' ') {
        s.remove_prefix(1);
    }
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    if (s.empty() || s.size() > max_emoji_name) {
        return {};
    }
    std::string name;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || (!std::isalnum(u) && c != '-' && c != '_')) {
            return {};
        }
        name += static_cast<char>(std::tolower(u));
    }
    return name;
}

std::filesystem::path emoji_file(const std::string& name) {
    return emoji_dir() / (name + ".art");
}

std::vector<std::string> emoji_names() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(emoji_dir(), ec)) {
        if (entry.path().extension() == ".art") {
            if (const std::string name = emoji_name(entry.path().stem().string()); !name.empty()) {
                names.push_back(name);
            }
        }
    }
    std::ranges::sort(names);
    return names;
}

void emoji_name_help(zchat::Chat& chat, std::string_view given) {
    chat.notice(std::format("{} is not an emoji name: use up to {} letters, digits, - and _.",
                            zchat::text::sanitize(given, 40), max_emoji_name));
}

void add_emoji(zchat::Chat& chat, std::string_view arg) {
    const auto space = arg.find(' ');
    std::string_view rest = space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    if (arg.empty() || rest.empty()) {
        chat.notice("Use /addemoji NAME [SIZE] [TEXTURE%] FILE to save a picture as an emoji, then /emoji NAME to send "
                    "it (SIZE and TEXTURE% as for /image).");
        return;
    }
    const std::string name = emoji_name(arg.substr(0, space));
    if (name.empty()) {
        emoji_name_help(chat, arg.substr(0, space));
        return;
    }
    const auto art = draw_image(chat, rest, "add");
    if (!art) {
        return;
    }
    if (emoji_dir().empty()) {
        chat.notice("Could not find a folder to save emoji in.");
        return;
    }
    std::error_code ec;
    const bool existed = std::filesystem::exists(emoji_file(name), ec);
    std::filesystem::create_directories(emoji_dir(), ec);
    std::ofstream out(emoji_file(name), std::ios::binary | std::ios::trunc);
    out << *art;
    if (!out.flush()) {
        chat.notice(std::format("Could not save the emoji in {}", emoji_dir().string()));
        return;
    }
    chat.notice(std::format("{} emoji {}: send it with /emoji {}", existed ? "Replaced" : "Saved", name, name));
}

void send_emoji(zchat::Chat& chat, std::string_view arg) {
    const std::string name = emoji_name(arg);
    if (name.empty()) {
        if (arg.find_first_not_of(' ') != std::string_view::npos) {
            emoji_name_help(chat, arg);
            return;
        }
        const auto names = emoji_names();
        if (names.empty()) {
            chat.notice("No emoji yet: save one with /addemoji NAME [SIZE] [TEXTURE%] FILE.");
            return;
        }
        std::string list;
        for (const auto& n : names) {
            list += list.empty() ? "" : ", ";
            list += n;
        }
        chat.notice(std::format("Your emoji: {}. Send one with /emoji NAME.", list));
        return;
    }
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(emoji_file(name), ec);
    if (ec) {
        chat.notice(std::format("There is no emoji {} (try /emoji).", name));
        return;
    }
    if (bytes > max_emoji_bytes) {
        chat.notice(std::format("The emoji {} is too big to send: add it again with /addemoji.", name));
        return;
    }
    std::ifstream in(emoji_file(name), std::ios::binary);
    const std::string art((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (art.empty()) {
        chat.notice(std::format("Could not read the emoji {}.", name));
        return;
    }
    chat.draw(art);
}

void remove_emoji(zchat::Chat& chat, std::string_view arg) {
    const std::string name = emoji_name(arg);
    if (name.empty()) {
        if (arg.find_first_not_of(' ') == std::string_view::npos) {
            chat.notice("Use /removeemoji NAME to delete an emoji (/emoji lists them).");
        } else {
            emoji_name_help(chat, arg);
        }
        return;
    }
    std::error_code ec;
    if (!std::filesystem::remove(emoji_file(name), ec)) {
        chat.notice(std::format("There is no emoji {} (try /emoji).", name));
        return;
    }
    chat.notice(std::format("Removed emoji {}.", name));
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

// Returns the exit code; restart is set when zchat was updated and should start again.
int run(const Options& options, zchat::Screen& terminal, bool& restart) {
    zchat::net::NetworkInit network;

    // --name wins over the nickname saved with /nick, which wins over a random name.
    std::mt19937_64 rng(std::random_device {}());
    std::string name;
    if (options.name) {
        name = *options.name;
    } else if (const auto saved = zchat::config::get("name"); saved && !clean_name(*saved).empty()) {
        name = clean_name(*saved);
    } else {
        name = zchat::random_name(rng);
    }

    const auto saved_color = zchat::config::get("color");
    zchat::Chat chat(options.port, name, saved_color ? zchat::parse_color(*saved_color) : std::nullopt, terminal);
#ifdef _WIN32
    active_chat = &chat;
    SetConsoleCtrlHandler(on_console_event, TRUE);
#endif

    // The last messages from before, like the scrollback of a chat, then the welcome.
    if (const auto earlier = zchat::history::load(); !earlier.empty()) {
        chat.notice(std::format("The last {} message{} from before:", earlier.size(), earlier.size() == 1 ? "" : "s"));
        chat.print_history(earlier);
    }
    chat.notice(std::format("Welcome to zchat! You are {}.", chat.colored_own_name()));
    chat.notice(
        std::format("Chatting on UDP port {}. Type a message and press Enter, /help for commands.", options.port));
    terminal.set_prompt(terminal.colors() ? "\x1b[1m>\x1b[0m " : "> ", 2);
    terminal.set_mentions([&chat] {
        return chat.mentionable();
    });
    terminal.set_self([&chat] {
        return zchat::Screen::Mention {chat.name(), zchat::ansi_foreground(chat.color())};
    });
    // Dropping an image file on the window types its path: show it as the /image command it becomes, which can
    // still be changed (e.g. given a size) before pressing Enter.
    terminal.set_rewriter([](std::string_view line) -> std::optional<std::string> {
        if (!zchat::image::is_dropped_image(line)) {
            return std::nullopt;
        }
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        while (!line.empty() && line.back() == ' ') {
            line.remove_suffix(1);
        }
        return std::format("/image {}", line);
    });
    zchat::game::Games games(chat, terminal);
    chat.start();

    // Declared after the chat and the terminal it uses, so that an update in progress is cancelled first.
    std::atomic<bool> updated {false};
    zchat::update::Updater updater(
        [&chat](std::string_view text) {
            chat.notice(text);
        },
        [&] {
            updated = true;
            terminal.interrupt();
        });

    while (!updated) {
        const auto line = terminal.read_line();
        if (!line) {
            break;
        }
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
        } else if (input == "/whoami") {
            chat.notice(std::format("You are {}.", chat.colored_own_name()));
        } else if (input == "/nick" || input.starts_with("/nick ")) {
            change_nick(chat, input.substr(std::min(input.size(), std::string_view("/nick ").size())));
        } else if (input == "/color" || input.starts_with("/color ")) {
            std::string_view arg = input.substr(std::min(input.size(), std::string_view("/color ").size()));
            while (!arg.empty() && arg.front() == ' ') {
                arg.remove_prefix(1);
            }
            change_color(chat, arg, rng);
        } else if (input == "/tags" || input == "/tag" || input.starts_with("/tags ") || input.starts_with("/tag ")) {
            const auto space = input.find(' ');
            print_tags(chat, space == std::string_view::npos ? std::string_view() : input.substr(space + 1),
                       terminal.colors());
        } else if (input == "/forget") {
            forget_nick(chat, rng);
        } else if (input == "/image" || input.starts_with("/image ")) {
            send_image(chat, input.substr(std::min(input.size(), std::string_view("/image ").size())));
        } else if (input == "/ascii" || input.starts_with("/ascii ")) {
            send_ascii(chat, input.substr(std::min(input.size(), std::string_view("/ascii ").size())));
        } else if (input == "/addemoji" || input.starts_with("/addemoji ")) {
            add_emoji(chat, input.substr(std::min(input.size(), std::string_view("/addemoji ").size())));
        } else if (input == "/removeemoji" || input.starts_with("/removeemoji ")) {
            remove_emoji(chat, input.substr(std::min(input.size(), std::string_view("/removeemoji ").size())));
        } else if (input == "/emoji" || input.starts_with("/emoji ")) {
            send_emoji(chat, input.substr(std::min(input.size(), std::string_view("/emoji ").size())));
        } else if (input == "/game" || input.starts_with("/game ")) {
            std::string_view arg = input.substr(std::min(input.size(), std::string_view("/game ").size()));
            while (!arg.empty() && arg.front() == ' ') {
                arg.remove_prefix(1);
            }
            games.command(arg);
        } else if (input == "/update") {
            if (!updater.start()) {
                chat.notice("An update is already in progress.");
            }
        } else if (input == "/help" || input == "/?") {
            print_help(chat);
        } else if (zchat::image::is_dropped_image(input)) {
            // Dropping a file on a terminal types its path: send the picture instead of the path. Checked before
            // the unknown commands, as a path can start with '/' too.
            send_image(chat, input);
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
    restart = updated;
    if (!restart) {
        chat.notice("Bye!");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    zchat::update::clean_up();
    int exit_code = 0;
    const auto options = parse_args(argc, argv, exit_code);
    if (!options) {
        return exit_code;
    }
    bool restart = false;
#ifdef ZCHAT_GUI
    // A window, unless asked for the terminal, or where there is no display (e.g. over ssh).
    if (!options->terminal && zchat::Gui::available()) {
#ifdef _WIN32
        // Started from Explorer, zchat gets a console window of its own: not needed with a window. (Started from a
        // console, it is not ours alone, and stays.)
        DWORD processes[2];
        if (GetConsoleProcessList(processes, 2) == 1) {
            FreeConsole();
        }
#endif
        zchat::Gui gui;
        gui.set_info(options->port);
        // The chat runs on its own thread; the window needs the main one (macOS requires it).
        std::jthread session([&] {
            try {
                exit_code = run(*options, gui, restart);
                gui.close();
            } catch (const std::exception& e) {
                // Left open, so the reason can be read.
                gui.print(std::format("zchat: {}", e.what()));
                exit_code = 1;
            }
        });
        gui.run();
        gui.interrupt();
        session.join();
        return restart ? zchat::update::restart(argv) : exit_code;
    }
#endif
    try {
        zchat::Terminal terminal;
        exit_code = run(*options, terminal, restart);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "zchat: %s\n", e.what());
        return 1;
    }
    // Only now that the terminal is back to normal and the chat socket is closed.
    return restart ? zchat::update::restart(argv) : exit_code;
}
