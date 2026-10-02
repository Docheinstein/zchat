#include "channel.hpp"
#include "chat.hpp"
#include "config.hpp"
#include "file.hpp"
#include "game.hpp"
#ifdef ZCHAT_GUI
#include "gui.hpp"
#endif
#include "history.hpp"
#include "image.hpp"
#include "markup.hpp"
#include "names.hpp"
#include "net.hpp"
#include "sound.hpp"
#include "terminal.hpp"
#include "text.hpp"
#include "trill_mp3.hpp"
#ifdef ZCHAT_TRILL_PICTURE
#include "trill_picture.hpp"
#endif
#include "update.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
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
#include <thread>
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

// Who we are for channels: an id of ours in the config, made the first time.
std::uint64_t user_id(std::mt19937_64& rng) {
    if (const auto saved = zchat::config::get("user")) {
        std::uint64_t id = 0;
        const auto [ptr, ec] = std::from_chars(saved->data(), saved->data() + saved->size(), id, 16);
        if (ec == std::errc {} && ptr == saved->data() + saved->size() && id != 0) {
            return id;
        }
    }
    std::uint64_t id = 0;
    while (id == 0) {
        id = rng();
    }
    zchat::config::set("user", std::format("{:x}", id));
    return id;
}

// "[#CHANNEL] REST": the channel, when named, and the rest.
std::pair<std::string_view, std::string_view> channel_and_rest(std::string_view arg) {
    while (!arg.empty() && arg.front() == ' ') {
        arg.remove_prefix(1);
    }
    if (!arg.starts_with('#')) {
        return {{}, arg};
    }
    const auto space = arg.find(' ');
    std::string_view rest = space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    return {arg.substr(0, space), rest};
}

void print_channels(zchat::Chat& chat) {
    std::string list;
    for (const auto& c : chat.channels()) {
        list += list.empty() ? "" : ", ";
        list += std::format("#{}{}{}{}", c.name, c.is_public ? "" : " (private)", c.current ? " (here)" : "",
                            c.unread ? " (new)" : c.member || c.name == zchat::channel::general ? "" : " (not in)");
    }
    chat.notice(std::format("Channels: {}.", list));
    chat.notice("/join NAME to go to one, /create NAME [public|private] for a new one.");
}

void print_members(zchat::Chat& chat, std::string_view arg) {
    const auto [channel, rest] = channel_and_rest(arg);
    const std::string name = channel.empty() ? chat.current_channel() : zchat::channel::clean_name(channel);
    std::size_t away = 0;
    auto names = chat.channel_members(name, away);
    if (names.empty() && away == 0) {
        chat.notice(std::format("There is no #{} for you (see /channels).", name));
        return;
    }
    std::ranges::sort(names);
    std::string list;
    for (const auto& n : names) {
        list += list.empty() ? "" : ", ";
        list += n;
    }
    chat.notice(std::format("In #{}: {}{}.", name, list.empty() ? "nobody here now" : list,
                            away ? std::format(", and {} not in the chat now", away) : ""));
}

// Runs a channel command: what went wrong, if anything, is said.
void channel_command(zchat::Chat& chat, std::string_view command, std::string_view arg) {
    while (!arg.empty() && arg.front() == ' ') {
        arg.remove_prefix(1);
    }
    std::optional<std::string> error;
    if (command == "/create") {
        const auto space = arg.find(' ');
        const std::string_view name = arg.substr(0, space);
        std::string_view kind = space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
        while (!kind.empty() && kind.front() == ' ') {
            kind.remove_prefix(1);
        }
        if (name.empty() || (!kind.empty() && kind != "public" && kind != "private")) {
            chat.notice("Use /create NAME [public|private]: public (the default) anybody can /join, private only "
                        "whoever you /add.");
            return;
        }
        error = chat.create_channel(name, kind != "private");
    } else if (command == "/join") {
        if (arg.empty()) {
            print_channels(chat);
            return;
        }
        error = chat.join_channel(arg);
    } else if (command == "/leave") {
        error = chat.leave_channel(arg);
    } else if (command == "/delete") {
        if (arg.empty()) {
            chat.notice("Use /delete NAME to delete a channel you created (#general can never be removed).");
            return;
        }
        error = chat.delete_channel(arg);
    } else if (command == "/add" || command == "/remove") {
        const auto [channel, person] = channel_and_rest(arg);
        if (person.empty()) {
            chat.notice(std::format("Use {} [#CHANNEL] NAME, with the name someone has in the chat; the channel is "
                                    "the one you are in when not named.",
                                    command));
            return;
        }
        error = command == "/add" ? chat.add_to_channel(channel, person) : chat.remove_from_channel(channel, person);
    }
    if (error) {
        chat.notice(*error);
    }
}

void print_help(zchat::Chat& chat) {
    chat.notice("People:");
    chat.notice("  /who         list the people in the chat");
    chat.notice("  @NAME        tag someone in a message: they hear a sound");
    chat.notice("               (type @ to pick from the list with Up/Down, then Enter or Tab)");
    chat.notice("  @everyone    tag all the people in the chat: they all hear a sound");
    chat.notice("You:");
    chat.notice("  /whoami      show your name");
    chat.notice("  /nick NAME   change your name, and keep it for next time");
    chat.notice("  /forget      forget the saved name and get a new random one");
    chat.notice("  /avatar FILE set your avatar, which everybody sees (a GIF plays); /avatar none removes it");
    chat.notice("  /color NAME  change the color of your name, and keep it for next time");
    chat.notice("               (a name from /color, #rrggbb, or r,g,b; /color random picks one, not kept)");
    chat.notice("Channels:");
    chat.notice("  /channels    list the channels; /join NAME goes to one (and joins it, if public)");
    chat.notice("  /create NAME [public|private]  make a channel: public anybody can join, private only who you add");
    chat.notice("  /add [#CHANNEL] NAME, /remove [#CHANNEL] NAME  add or remove someone (the current channel if none)");
    chat.notice("  /members [#CHANNEL]  who is in a channel; /leave [NAME] leaves one; /delete NAME deletes yours");
    chat.notice("Messages, pictures and files:");
    chat.notice("  /tags [NAME] list the tags for messages, like <color=red>text</color>, or explain one");
    chat.notice("  /image [SIZE] FILE  send a picture (windows show it, terminals draw it with characters)");
    chat.notice("  /ascii [SIZE] [TEXTURE%] FILE  send a picture drawn with characters, for everyone");
    chat.notice("               (SIZE: small, medium, large, a width like 40, or 40x20;");
    chat.notice("                TEXTURE%: 0% blocks only, the default, to 100% symbols only)");
    chat.notice("               (or drop an image file on the window, then press Enter)");
    chat.notice("  /addemoji NAME [SIZE] FILE  save a picture as an emoji (a GIF plays)");
    chat.notice("  /addemoji NAME ascii [SIZE] [TEXTURE%] FILE  save one drawn with characters");
    chat.notice("  /emoji NAME [ascii]  send a saved emoji, or draw it with characters (/emoji alone lists yours)");
    chat.notice("  /removeemoji NAME  delete a saved emoji");
    chat.notice("  /file FILE   send a file of any kind, for everyone to download");
    chat.notice("               (or drop any other file on the window, then press Enter)");
    chat.notice("  /save N      save file N from the chat in your downloads folder (/save alone lists them)");
    chat.notice("Fun:");
    chat.notice("  /trill [SOUND] [PICTURE]  trill everybody else: they have 3 seconds to catch a STOP button running");
    chat.notice(std::format("               around the screen, or they hear the sound (MP3, WAV, up to {}) at full "
                            "volume",
                            zchat::file::format_size(zchat::sound::max_bytes)));
    chat.notice("               and see the picture flying around (/trill alone: Fahhh); it costs coins (/shop)");
    chat.notice("  /stop        stop the trills coming at you");
    chat.notice("  /kick NAME   ask the others to vote NAME out of the chat: /kick yes kicks, /kick no graces;");
    chat.notice("               they are out (their zchat closes) if at least as many vote to kick as to grace;");
    chat.notice("               asking costs coins (/shop), voting is free");
    chat.notice("  /spy @NAME   ask NAME to let you see their screen(s): if they /spy allow (or do not answer in");
    chat.notice("               time), their zchat shares them; /spy deny refuses and nothing is captured. Asking is free:");
    chat.notice("               only if they accept do you pay 10 coins, and they get 5 for letting you in");
    chat.notice("  /game        list the games everyone in the chat can play (/game help NAME: how to play one)");
    chat.notice("  /coins [NAME]  your coins, or somebody's: won in the games, spent on /trill, /kick and /spy");
    chat.notice("               (/coins top: who has the most; /shop: the prices)");
    chat.notice("  /casino      bet your coins at blackjack, roulette and the horse race, with the whole chat");
    chat.notice("               (in its own window; /casino help: how to play in the chat)");
    chat.notice("zchat:");
    chat.notice("  /update      get the latest zchat, build it and restart");
    chat.notice("  /help        show this help (/help game: the games)");
    chat.notice("  /quit        leave the chat (or Ctrl+C, Ctrl+D)");
}

// The avatar: the picture as it is sent (see image::encode_avatar()), in the config folder, so it is back next time.
std::filesystem::path avatar_file() {
    const auto dir = zchat::config::dir();
    return dir.empty() ? dir : dir / "avatar";
}

std::optional<std::string> load_avatar() {
    const auto file = avatar_file();
    std::error_code ec;
    if (file.empty() || std::filesystem::file_size(file, ec) > zchat::max_avatar_bytes || ec) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        return std::nullopt;
    }
    return zchat::image::parse_picture(text) ? std::optional<std::string>(std::move(text)) : std::nullopt;
}

void change_avatar(zchat::Chat& chat, std::string_view arg) {
    while (!arg.empty() && arg.front() == ' ') {
        arg.remove_prefix(1);
    }
    if (arg.empty()) {
        chat.notice(chat.own_avatar_hash().empty()
                        ? "You have no avatar. Use /avatar FILE to set one (a GIF plays), /avatar none to remove it."
                        : "You have an avatar. Use /avatar FILE to change it, /avatar none to remove it.");
        return;
    }
    const auto file = avatar_file();
    if (arg == "none" || arg == "remove") {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        chat.set_avatar(std::nullopt);
        chat.notice("Your avatar is removed.");
        return;
    }
    const auto avatar = zchat::image::encode_avatar(zchat::image::parse_path(arg));
    if (!avatar) {
        chat.notice(std::format("Cannot use {} as your avatar: {}", zchat::text::sanitize(arg, 200), avatar.error()));
        return;
    }
    chat.set_avatar(*avatar);
    chat.notice("Your avatar is set: everybody sees it in a moment.");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(avatar->data(), static_cast<std::streamsize>(avatar->size()));
    if (file.empty() || !out.flush()) {
        chat.notice("Could not save your avatar for next time.");
    }
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

// Turns parsed "[SIZE] FILE" into a real picture (see image::encode_picture()), or says why it cannot
// ("Cannot <what> FILE: ...").
std::optional<std::string> make_picture(zchat::Chat& chat, const ImageArgs& args, std::string_view what) {
    const int size = std::min(max_picture_size, static_cast<int>(std::min(args.size.cols * picture_pixels_per_char,
                                                                          args.size.rows * 2 * picture_pixels_per_char)));
    bool still = false;
    auto picture = zchat::image::encode_picture(zchat::image::parse_path(args.file), size, &still);
    if (!picture) {
        chat.notice(std::format("Cannot {} {}: {}", what, zchat::text::sanitize(args.file, 200), picture.error()));
        return std::nullopt;
    }
    if (still) {
        chat.notice("That animation is too long to keep whole: using its first frame.");
    }
    return std::move(*picture);
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
    if (const auto picture = make_picture(chat, *args, "send")) {
        chat.send_picture(*picture);
    }
}

void send_file(zchat::Chat& chat, std::string_view arg) {
    if (arg.empty()) {
        chat.notice(std::format("Use /file FILE to send a file of any kind (up to {}), or drop it on the window and "
                                "press Enter. Everyone can then save it with /save.",
                                zchat::file::format_size(zchat::file::max_bytes())));
        return;
    }
    const auto text = zchat::file::encode(zchat::image::parse_path(arg));
    if (!text) {
        chat.notice(std::format("Cannot send {}: {}", zchat::text::sanitize(arg, 200), text.error()));
        return;
    }
    chat.send_file(*text);
}

// The pictures of trills: at most this many pixels wide and tall when they have to be made again (see
// image::encode_picture_data()).
constexpr int max_trill_picture_size = 1280;

// The one or two files of "/trill FILE [FILE]": the whole line when it is a file, or else its two halves at the first
// space where both are. Empty when there are none.
std::vector<std::filesystem::path> trill_files(std::string_view arg) {
    const auto exists = [](const std::filesystem::path& path) {
        std::error_code ec;
        return !path.empty() && std::filesystem::is_regular_file(path, ec);
    };
    if (auto whole = zchat::image::parse_path(arg); exists(whole)) {
        return {std::move(whole)};
    }
    for (auto space = arg.find(' '); space != std::string_view::npos; space = arg.find(' ', space + 1)) {
        auto first = zchat::image::parse_path(arg.substr(0, space));
        auto second = zchat::image::parse_path(arg.substr(space + 1));
        if (exists(first) && exists(second)) {
            return {std::move(first), std::move(second)};
        }
    }
    return {};
}

// A trill: a sound (MP3 or WAV) and a picture, or one of them, that everybody else in the chat (or the channel) gets
// once, after a few seconds to stop it; without either, Fahhh. It costs coins (see coins.hpp), more with our own
// sound or picture, paid once it is ready to go.
void send_trill(zchat::Chat& chat, zchat::game::Games& games, std::string_view arg) {
    zchat::file::Trill trill;
    if (arg.empty()) {
        trill.sound_name = "Fahhh.mp3";
        trill.sound.assign(reinterpret_cast<const char*>(trill_mp3), trill_mp3_size);
#ifdef ZCHAT_TRILL_PICTURE
        if (auto picture = zchat::image::encode_picture_data(
                std::string_view(reinterpret_cast<const char*>(trill_picture), trill_picture_size),
                max_trill_picture_size, zchat::file::max_trill_picture_bytes)) {
            trill.picture_name = "Fahhh.jpg";
            trill.picture = std::move(*picture);
        }
#endif
        if (games.spend("trill")) {
            chat.send_file(zchat::file::encode_trill(trill));
        }
        return;
    }
    const auto files = trill_files(arg);
    if (files.empty()) {
        chat.notice(std::format("Cannot find {}: use /trill [SOUND] [PICTURE], with a sound (MP3 or WAV), a picture or "
                                "both.",
                                zchat::text::sanitize(arg, 200)));
        return;
    }
    for (const auto& path : files) {
        const auto u8 = path.filename().u8string();
        const std::string name(u8.begin(), u8.end());
        const std::string shown = zchat::text::sanitize(name, 200);
        auto data = zchat::image::read_image_file(path);
        if (!data) {
            chat.notice(std::format("Cannot send {}: {}", shown, data.error()));
            return;
        }
        if (zchat::sound::is_audio(*data)) {
            if (!trill.sound.empty()) {
                chat.notice("A trill has one sound: give a sound, a picture, or one of each.");
                return;
            }
            if (data->size() > zchat::sound::max_bytes) {
                chat.notice(std::format("Cannot send {}: sounds up to {} can be trilled.", shown,
                                        zchat::file::format_size(zchat::sound::max_bytes)));
                return;
            }
            trill.sound_name = name;
            trill.sound = std::move(*data);
            continue;
        }
        auto picture =
            zchat::image::encode_picture_data(*data, max_trill_picture_size, zchat::file::max_trill_picture_bytes);
        if (!picture) {
            chat.notice(std::format("Cannot send {}: {} Sounds can be MP3 or WAV files.", shown, picture.error()));
            return;
        }
        if (!trill.picture.empty()) {
            chat.notice("A trill has one picture: give a sound, a picture, or one of each.");
            return;
        }
        trill.picture_name = name;
        trill.picture = std::move(*picture);
    }
    if (games.spend("custom trill")) {
        chat.send_file(zchat::file::encode_trill(trill));
    }
}

void save_file(zchat::Chat& chat, std::string_view arg) {
    const std::size_t count = chat.received_files();
    if (arg.empty()) {
        if (count == 0) {
            chat.notice("No files in the chat yet. When somebody sends one, /save N saves it in your downloads "
                        "folder.");
            return;
        }
        chat.notice("The files in the chat (/save N saves one in your downloads folder):");
        for (std::size_t i = 1; i <= count; ++i) {
            if (const auto f = chat.received_file(i)) {
                std::error_code ec;
                const auto size = std::filesystem::file_size(f->path, ec);
                chat.notice(std::format("  {}  {} ({})", i, f->name, zchat::file::format_size(ec ? 0 : size)));
            }
        }
        return;
    }
    std::size_t index = 0;
    const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), index);
    const auto f = ec == std::errc {} && ptr == arg.data() + arg.size() ? chat.received_file(index) : std::nullopt;
    if (!f) {
        chat.notice(count == 0 ? std::string("No files in the chat yet.")
                               : std::format("There is no file {}: use a number from 1 to {} (/save lists them).",
                                             zchat::text::sanitize(arg, 32), count));
        return;
    }
    const auto saved = zchat::file::copy_into(f->path, zchat::file::downloads_dir(), f->name);
    if (!saved) {
        chat.notice(std::format("Cannot save {}: {}", f->name, saved.error()));
        return;
    }
    const auto u8 = saved->u8string();
    const std::string where(reinterpret_cast<const char*>(u8.data()), u8.size());
    chat.notice(std::format("Saved {} to {}", f->name, zchat::text::sanitize(where, 500)));
}

// Emoji: pictures saved under a name in the emoji folder of the config folder, so /emoji NAME sends them without the
// image file. Each is a real picture, NAME.pic (as /image sends it, see image::encode_picture()), or a picture drawn
// with characters, NAME.art (as /ascii sends it).
constexpr std::size_t max_emoji_name = 32;
constexpr std::uintmax_t max_emoji_art_bytes = 64 * 1024;

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

// ascii: the drawing (NAME.art), or else the picture (NAME.pic).
std::filesystem::path emoji_file(const std::string& name, bool ascii) {
    return emoji_dir() / (name + (ascii ? ".art" : ".pic"));
}

// A saved emoji: what it is, and whether it is drawn with characters.
struct Emoji {
    std::string data;
    bool ascii = false;
};

std::optional<Emoji> load_emoji(const std::string& name) {
    for (const bool ascii : {false, true}) {
        std::error_code ec;
        const auto file = emoji_file(name, ascii);
        const auto bytes = std::filesystem::file_size(file, ec);
        if (ec || bytes > (ascii ? max_emoji_art_bytes : zchat::max_image_bytes)) {
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!data.empty()) {
            return Emoji {std::move(data), ascii};
        }
    }
    return std::nullopt;
}

// The emoji saved, sorted, each with "(ascii)" when it is drawn with characters.
std::vector<std::string> emoji_names() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(emoji_dir(), ec)) {
        const auto extension = entry.path().extension();
        if (extension == ".art" || extension == ".pic") {
            if (const std::string name = emoji_name(entry.path().stem().string()); !name.empty()) {
                names.push_back(extension == ".art" ? name + " (ascii)" : name);
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

// Takes the word "ascii" off the front of arg, telling whether it was there.
bool take_ascii(std::string_view& arg) {
    while (!arg.empty() && arg.front() == ' ') {
        arg.remove_prefix(1);
    }
    if (arg == "ascii" || arg.starts_with("ascii ")) {
        arg.remove_prefix(std::min(arg.size(), std::string_view("ascii ").size()));
        while (!arg.empty() && arg.front() == ' ') {
            arg.remove_prefix(1);
        }
        return true;
    }
    return false;
}

void add_emoji_help(zchat::Chat& chat) {
    chat.notice("Use /addemoji NAME [SIZE] FILE to save a picture as an emoji (a GIF plays), then /emoji NAME to send "
                "it; SIZE as for /image.");
    chat.notice("Use /addemoji NAME ascii [SIZE] [TEXTURE%] FILE to save it drawn with characters instead, as /ascii "
                "draws (a TEXTURE% alone does too).");
}

void add_emoji(zchat::Chat& chat, std::string_view arg) {
    const auto space = arg.find(' ');
    std::string_view rest = space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
    bool ascii = take_ascii(rest);
    if (arg.empty() || rest.empty()) {
        add_emoji_help(chat);
        return;
    }
    const std::string name = emoji_name(arg.substr(0, space));
    if (name.empty()) {
        emoji_name_help(chat, arg.substr(0, space));
        return;
    }
    const auto args = parse_image_args(chat, rest);
    if (!args) {
        return;
    }
    // A texture is for pictures drawn with characters, as with /image.
    ascii = ascii || args->texture.has_value();
    const auto data = ascii ? draw_image(chat, rest, "add") : make_picture(chat, *args, "add");
    if (!data) {
        return;
    }
    if (emoji_dir().empty()) {
        chat.notice("Could not find a folder to save emoji in.");
        return;
    }
    std::error_code ec;
    const bool existed = std::filesystem::exists(emoji_file(name, false), ec) ||
                         std::filesystem::exists(emoji_file(name, true), ec);
    std::filesystem::create_directories(emoji_dir(), ec);
    std::ofstream out(emoji_file(name, ascii), std::ios::binary | std::ios::trunc);
    out.write(data->data(), static_cast<std::streamsize>(data->size()));
    if (!out.flush()) {
        chat.notice(std::format("Could not save the emoji in {}", emoji_dir().string()));
        return;
    }
    out.close();
    // One emoji per name: the other kind goes.
    std::filesystem::remove(emoji_file(name, !ascii), ec);
    chat.notice(std::format("{} emoji {}{}: send it with /emoji {}", existed ? "Replaced" : "Saved", name,
                            ascii ? " (drawn with characters)" : "", name));
}

void send_emoji(zchat::Chat& chat, std::string_view arg) {
    // "/emoji NAME ascii" draws a picture emoji with characters, this once.
    while (!arg.empty() && arg.back() == ' ') {
        arg.remove_suffix(1);
    }
    bool as_ascii = false;
    if (const auto space = arg.rfind(' '); space != std::string_view::npos && arg.substr(space + 1) == "ascii") {
        as_ascii = true;
        arg = arg.substr(0, space);
    }
    const std::string name = emoji_name(arg);
    if (name.empty()) {
        if (arg.find_first_not_of(' ') != std::string_view::npos) {
            emoji_name_help(chat, arg);
            return;
        }
        const auto names = emoji_names();
        if (names.empty()) {
            chat.notice("No emoji yet: save one with /addemoji NAME [SIZE] FILE (or /addemoji NAME ascii ... for one "
                        "drawn with characters).");
            return;
        }
        std::string list;
        for (const auto& n : names) {
            list += list.empty() ? "" : ", ";
            list += n;
        }
        chat.notice(std::format("Your emoji: {}. Send one with /emoji NAME (or /emoji NAME ascii to draw it with "
                                "characters).",
                                list));
        return;
    }
    const auto emoji = load_emoji(name);
    if (!emoji) {
        chat.notice(std::format("There is no emoji {} (try /emoji).", name));
        return;
    }
    if (emoji->ascii) {
        chat.draw(emoji->data);
        return;
    }
    if (!as_ascii) {
        chat.send_picture(emoji->data);
        return;
    }
    // Its first frame, drawn as /ascii draws a picture as big as it.
    const auto picture = zchat::image::parse_picture(emoji->data);
    const auto art =
        picture ? zchat::image::to_ascii_data(picture->first,
                                              std::clamp<std::size_t>(static_cast<std::size_t>(picture->width) /
                                                                          picture_pixels_per_char,
                                                                      8, zchat::max_art_cols),
                                              zchat::max_art_rows)
                : std::unexpected(std::string("it cannot be read"));
    if (!art) {
        chat.notice(std::format("Cannot draw the emoji {}: {}", name, art.error()));
        return;
    }
    chat.draw(*art);
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
    const bool picture = std::filesystem::remove(emoji_file(name, false), ec);
    const bool drawing = std::filesystem::remove(emoji_file(name, true), ec);
    if (!picture && !drawing) {
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
    if (const auto dir = zchat::config::dir(); !dir.empty()) {
        chat.set_user(user_id(rng), dir / "channels");
    }
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
        return zchat::Screen::Mention {chat.name(), zchat::ansi_foreground(chat.color()), chat.own_avatar_hash()};
    });
    terminal.set_avatars([&chat](std::string_view hash) {
        return chat.avatar(hash);
    });
    terminal.set_channels([&chat] {
        std::vector<zchat::Screen::ChannelItem> items;
        for (const auto& c : chat.channels()) {
            items.push_back({c.name, c.is_public, c.member, c.current, c.unread, c.owner});
        }
        return items;
    });
    terminal.set_channel_people([&chat](std::string_view channel) {
        auto people = chat.channel_people(channel);
        return zchat::Screen::ChannelPeople {std::move(people.members), std::move(people.others), people.away};
    });
    // Dropping a file on the window types its path: show it as the /image command it becomes, which can still be
    // changed (e.g. given a size) before pressing Enter; or /file, for other kinds of file.
    terminal.set_rewriter([](std::string_view line) -> std::optional<std::string> {
        const bool picture = zchat::image::is_dropped_image(line);
        if (!picture && !zchat::file::is_dropped_file(line)) {
            return std::nullopt;
        }
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        while (!line.empty() && line.back() == ' ') {
            line.remove_suffix(1);
        }
        return std::format("{} {}", picture ? "/image" : "/file", line);
    });
    // Voted out of the chat with /kick: zchat closes.
    std::atomic<bool> kicked {false};
    zchat::game::Games games(chat, terminal, [&] {
        kicked = true;
        terminal.interrupt();
    });
    chat.start();
    // After the Join, which must come first: the others learn about it from the heartbeat it sends.
    if (auto avatar = load_avatar()) {
        chat.set_avatar(std::move(*avatar));
    }

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

    while (!updated && !kicked) {
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
        const auto command = input.substr(0, input.find(' '));
        if (command == "/create" || command == "/join" || command == "/leave" || command == "/delete" ||
            command == "/add" || command == "/remove") {
            channel_command(chat, command, input.substr(command.size()));
            continue;
        }
        if (input == "/channels") {
            print_channels(chat);
            continue;
        }
        if (input == "/members" || input.starts_with("/members ")) {
            print_members(chat, input.substr(std::string_view("/members").size()));
            continue;
        }
        if (input == "/who" || input == "/list") {
            print_who(chat);
        } else if (input == "/whoami") {
            chat.notice(std::format("You are {}.", chat.colored_own_name()));
        } else if (input == "/avatar" || input.starts_with("/avatar ")) {
            change_avatar(chat, input.substr(std::min(input.size(), std::string_view("/avatar").size())));
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
        } else if (input == "/file" || input.starts_with("/file ")) {
            send_file(chat, input.substr(std::min(input.size(), std::string_view("/file ").size())));
        } else if (input == "/save" || input.starts_with("/save ")) {
            save_file(chat, input.substr(std::min(input.size(), std::string_view("/save ").size())));
        } else if (input == "/trill" || input.starts_with("/trill ")) {
            send_trill(chat, games, input.substr(std::min(input.size(), std::string_view("/trill ").size())));
        } else if (input == "/stop") {
            const std::size_t stopped = chat.stop_trills();
            chat.notice(stopped == 0 ? std::string("No trill to stop.")
                                     : std::format("Stopped {} trill{}.", stopped, stopped == 1 ? "" : "s"));
        } else if (input == "/kick" || input.starts_with("/kick ")) {
            games.kick(input.substr(std::min(input.size(), std::string_view("/kick ").size())));
        } else if (input == "/spy" || input.starts_with("/spy ")) {
            games.spy(input.substr(std::min(input.size(), std::string_view("/spy ").size())));
        } else if (input == "/coins" || input.starts_with("/coins ")) {
            games.coins(input.substr(std::min(input.size(), std::string_view("/coins ").size())));
        } else if (input == "/shop") {
            games.coins("shop");
        } else if (input == "/casino" || input.starts_with("/casino ")) {
            games.casino(input.substr(std::min(input.size(), std::string_view("/casino ").size())));
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
        } else if (input.starts_with("/help ")) {
            // /help game [NAME]: the games' own help (see Games::command()).
            std::string_view topic = input.substr(std::string_view("/help ").size());
            while (!topic.empty() && topic.front() == ' ') {
                topic.remove_prefix(1);
            }
            if (topic == "game" || topic.starts_with("game ")) {
                games.command(std::format("help {}", topic.substr(std::min(topic.size(), std::size_t {5}))));
            } else {
                chat.notice(std::format("No help about {}: /help lists the commands, /help game the games.",
                                        zchat::text::sanitize(topic, 32)));
            }
        } else if (zchat::image::is_dropped_image(input)) {
            // Dropping a file on a terminal types its path: send the picture instead of the path. Checked before
            // the unknown commands, as a path can start with '/' too.
            send_image(chat, input);
        } else if (zchat::file::is_dropped_file(input)) {
            send_file(chat, input);
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
    if (kicked) {
        // Long enough to read why, before the window closes.
        std::this_thread::sleep_for(std::chrono::seconds(3));
    } else if (!restart) {
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
