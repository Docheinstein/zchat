#pragma once

#include "channel.hpp"
#include "color.hpp"
#include "file.hpp"
#include "history.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "screen.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zchat {

// A chat room made of every zchat instance listening on the same UDP port of the local networks.
// There is no server: every peer broadcasts its packets, and keeps track of the others from what it hears.
class Chat {
public:
    // Without a color, a random one is used.
    Chat(std::uint16_t port, std::string name, std::optional<Color> color, Screen& terminal);
    ~Chat();
    Chat(const Chat&) = delete;
    Chat& operator=(const Chat&) = delete;

    std::string name() const {
        std::scoped_lock lock(name_mutex_);
        return name_;
    }

    // Changes our name; the others see the change right away.
    void set_name(std::string name);

    Color color() const {
        return color_of_id(id_);
    }

    // Changes our name color; the others see the change right away.
    void set_color(Color color);

    // Formats text in a color.
    std::string paint(Color color, std::string_view text) const;

    // Announces this peer and starts listening.
    void start();

    // Sends a chat line to everybody.
    void say(std::string_view text);

    // Sends a picture drawn with characters to everybody: its rows, separated by '\n'.
    void draw(std::string_view art);

    // Sends a real picture to everybody (see image::encode_picture()): windows show it, terminals draw it.
    void send_picture(std::string_view picture);

    // Sends a file of any kind to everybody (see file::encode()), for them to download; or a trill, a sound they all
    // play once (see file::Kind::Trill).
    void send_file(std::string_view file);

    // A file received (or sent) in this session: where it is kept until zchat quits, and its name.
    struct ReceivedFile {
        std::filesystem::path path;
        std::string name;
    };
    // The file numbered index (from 1, in the order they came), as "/save INDEX" names it.
    std::optional<ReceivedFile> received_file(std::size_t index) const;
    std::size_t received_files() const;

    // Stops the trills coming at us or playing (see /trill); returns how many.
    std::size_t stop_trills();

    // Sets our avatar (see image::encode_avatar()), or none; the others download it from us, see presence_text().
    void set_avatar(std::optional<std::string> picture);

    // Our avatar's hash, in hex; empty for none.
    std::string own_avatar_hash() const;

    // An avatar downloaded (or ours), by hash in hex. Thread-safe.
    std::optional<std::string> avatar(std::string_view hash) const;

    // Who we are for channels: an id of ours that stays the same (unlike the sender id, which changes with the
    // color), kept in the config; and the file where the channels we know are kept. Before start().
    void set_user(std::uint64_t user, std::filesystem::path channels_file);

    // Our id of set_user(); 0 when there is none (no config folder).
    std::uint64_t user() const {
        return user_;
    }

    // Channels (see channel.hpp): what we say, draw and send goes to the current one. Each returns what went wrong, or
    // nullopt. The channel of add_to_channel() and the others is the current one when empty; people are named as
    // they are in the chat (they must be online).
    std::optional<std::string> create_channel(std::string_view name, bool is_public);
    std::optional<std::string> join_channel(std::string_view name);
    std::optional<std::string> leave_channel(std::string_view name);
    std::optional<std::string> delete_channel(std::string_view name);
    std::optional<std::string> add_to_channel(std::string_view channel, std::string_view person);
    std::optional<std::string> remove_from_channel(std::string_view channel, std::string_view person);
    std::string current_channel() const;
    struct ChannelInfo {
        std::string name;
        bool is_public = true;
        bool member = false;
        bool current = false;
        bool unread = false;
        std::size_t members = 0;
        // We created it: we can delete it.
        bool owner = false;
    };
    // General first, then the others there are for us: the public ones, and the private ones we are in.
    std::vector<ChannelInfo> channels() const;
    // The names of the members in the chat; away is how many others there are.
    std::vector<std::string> channel_members(std::string_view channel, std::size_t& away) const;
    // For the window: the members in the chat (us too), the others in the chat who could be added, and how many
    // members are not in the chat now. Names as they are, for /add and /remove.
    struct ChannelPeople {
        std::vector<Screen::Mention> members;
        std::vector<Screen::Mention> others;
        std::size_t away = 0;
    };
    ChannelPeople channel_people(std::string_view channel) const;

    // Tells the others this peer is leaving and stops listening. Safe to call more than once.
    void stop();

    // The names of the other peers currently in the chat.
    std::vector<std::string> peers() const;

    // The other peers currently in the chat, by id, with their names as they are (not colored).
    std::vector<std::pair<std::uint64_t, std::string>> people() const;

    // The other peers, sorted by name, for the list shown when typing '@'.
    std::vector<Screen::Mention> mentionable() const;

    // Our own name, with its color.
    std::string colored_own_name() const {
        return colored_name(id_, name());
    }

    // Formats a name with its color.
    std::string colored_name(std::uint64_t id, std::string_view name) const;

    // Prints an informational line (joins, leaves, command output).
    void notice(std::string_view text) const;

    // What the games say: a notice, or a full line (a board drawn with characters). In a window they go to the games
    // log (see channel::games_log), so the chat stays a chat; a terminal shows them in the chat. Thread-safe.
    void game_notice(std::string_view text);
    void game_print(std::string_view line);

    // Prints messages from the history (see history.hpp), each with the time it was sent at.
    void print_history(const std::vector<history::Entry>& entries);

    // Our id, which changes with the color.
    std::uint64_t id() const {
        return id_;
    }

    // What the games played in the chat hear about, see game::Games. Called on the chat thread, but a message of
    // ours on the thread that said it; never at the same time.
    struct GameHooks {
        // A Game packet from another peer.
        std::function<void(std::uint64_t sender, std::string_view name, std::string_view text)> packet;
        // Every chat line, ours included, after it is printed.
        std::function<void(std::uint64_t sender, std::string_view name, std::string_view text)> message;
        // Called a few times per second, for the game's timers.
        std::function<void()> tick;
        // Whether a chat line of another peer is a move of a game (a race's words): where the games have a log of
        // their own, it is shown there, and not in #general.
        std::function<bool(std::uint64_t sender, std::string_view text)> claims;
    };

    // Replaces the game hooks; empty ones are not called. Once it returns, the old ones are not running anymore.
    void set_game_hooks(GameHooks hooks);

    // Sends a Game packet to everybody (not to us).
    void send_game(std::string_view text);

    // While a game is played in real time (see the bomber, arena and frag games), the game hooks tick about 50 times a
    // second instead of a few. Each game holds its own bit, so one ending does not slow down another. Thread-safe.
    void set_fast_ticks(bool fast, unsigned bit = 1) {
        if (fast) {
            fast_ticks_ |= bit;
        } else {
            fast_ticks_ &= ~bit;
        }
    }

private:
    using clock = std::chrono::steady_clock;

    struct Peer {
        std::string name;
        clock::time_point last_seen;
        std::deque<std::uint64_t> recent_seqs;
        // Their avatar's hash, 0 for none.
        std::uint64_t avatar = 0;
        // Who they are for channels, see set_user(); 0 for versions without channels.
        std::uint64_t user = 0;
    };

    // Something said, drawn or sent in a channel, kept to show it again (see deliver()).
    struct Entry {
        // A Line is printed as it is: what a game said, in the games log.
        enum class Kind { Message, Art, Picture, File, Trill, Line };
        Kind kind = Kind::Message;
        std::time_t time = 0;
        std::uint64_t id = 0;
        std::string name;
        // The message, the drawing or the picture.
        std::shared_ptr<const std::string> data;
        // A file: see keep_file(); a trill: what it has, in file_name (no index, it is not kept).
        std::size_t file_index = 0;
        std::string file_name;
        std::string file_size;
    };

    void run(std::stop_token stop);
    // from is the address the packet came from.
    void handle(const Packet& packet, std::uint32_t from);
    // With once, to each network once, see net::BroadcastSocket::broadcast().
    void send(PacketType type, std::string_view text = {}, bool once = false);
    // Returns whether the message tags us. when is the time it was sent at, for messages from the history.
    bool print_message(std::uint64_t id, std::string_view name, std::string_view text,
                       std::optional<std::time_t> when = std::nullopt) const;
    // Turns the "@Name" tags of the people in the chat into markup showing them bold in their color, ours also
    // underlined. Sets tags_us when we are tagged.
    std::string mark_mentions(std::string_view text, bool& tags_us) const;
    void print_art(std::uint64_t id, std::string_view name, std::string_view art,
                   std::optional<std::time_t> when = std::nullopt) const;
    // Also files, which come the same way.
    void print_picture(std::uint64_t id, std::string_view name, std::string_view text,
                       std::optional<std::time_t> when = std::nullopt) const;
    void print_file(const Entry& entry, std::optional<std::time_t> when = std::nullopt) const;
    // A picture or file arrived (or ours): to the channel its first line names, see send_picture().
    void receive_picture(std::uint64_t id, std::string_view name, std::string_view text);
    // Saves a file received for /save, and fills in entry for it.
    bool keep_file(std::uint64_t id, std::string_view name, std::string_view text, Entry& entry);
    // A trill arrived: fills in entry for it, and plays it on a thread of its own (see run_trill()). Ours (from id) is
    // only shown: the others get it.
    bool receive_trill(std::uint64_t id, std::string_view name, std::string_view text, Entry& entry);
    // Gives the user three seconds to catch the STOP button running around the screen (or to type /stop); then shakes
    // the window, plays the sound at full volume and sends the picture flying around the screen.
    void run_trill(std::uint64_t id, const std::string& name, const file::Trill& trill, std::stop_token stop);
    // Where the sound of a trill is kept, by name: saved in the temporary folder the first time only. Empty when it
    // cannot be.
    std::filesystem::path trill_file(std::string_view name, std::string_view data);
    void print_trill(const Entry& entry, std::optional<std::time_t> when = std::nullopt) const;
    // Shows an entry: live when it just arrived (tags ring), or else again, with the time it came at.
    void show(const Entry& entry, bool live);
    // Something for a channel: kept, and shown if it is the current one, or else told about once.
    void deliver(const std::string& channel, Entry entry);
    // A notice as it is printed, with the time.
    std::string notice_line(std::string_view text) const;
    // Whether the games log is being shown; and the channel what we say, draw and send goes to: the current one, or
    // #general from the games log.
    bool in_games_log() const;
    std::string speaking_channel() const;
    // Channels: whether we are in one, who a user is, and who someone named is.
    bool in_channel(const std::string& name) const;
    std::string user_name(std::uint64_t user) const;
    std::optional<std::uint64_t> find_user(std::string_view person, std::string& error) const;
    // Makes a new version of a channel with edit (which returns what is wrong, if anything), and tells everybody.
    std::optional<std::string> change_channel(const std::string& name,
                                              const std::function<std::optional<std::string>(channel::Channel&)>& edit);
    void switch_to(const std::string& name);
    void receive_channel_state(const Packet& packet);
    void send_channel_states();
    void save_channels() const;
    void prune_silent_peers();
    // Pictures too big for a packet, see send_picture(): offered, and downloaded by the others from us, on threads
    // of their own (see start_transfer()); or else sent in Chunks, asked for again when some do not arrive.
    struct Incoming;
    using IncomingKey = std::pair<std::uint64_t, std::uint64_t>;
    bool start_transfer(std::function<void(std::stop_token)> work);
    void serve_pictures(std::stop_token stop);
    void serve_picture(net::TcpStream& stream, std::stop_token stop);
    void receive_offer(const Packet& packet, std::uint32_t from);
    void download_picture(IncomingKey key, std::uint32_t from, std::uint16_t port, std::size_t bytes,
                          std::stop_token stop);
    void finish_downloads();
    void finish_picture(std::map<IncomingKey, Incoming>::iterator it, std::string_view text);
    void send_chunk(std::uint64_t picture, std::size_t index, std::size_t count, std::string_view piece);
    void receive_chunk(const Packet& packet);
    void request_missing_chunks();
    void resend_chunks(std::string_view request);
    // Avatars: what our heartbeats say about ours ("avatar HASH BYTES PORT"), and downloading the others'.
    std::string presence_text() const;
    void receive_presence(std::uint64_t sender, std::string_view text, std::uint32_t from);
    void download_avatar(std::uint64_t hash, std::uint32_t from, std::uint16_t port, std::size_t bytes,
                         std::stop_token stop);

    // Changes with the color, see set_color().
    std::atomic<std::uint64_t> id_;
    mutable std::mutex name_mutex_;
    std::string name_;
    Screen& terminal_;
    net::BroadcastSocket socket_;
    std::atomic<std::uint64_t> seq_ {0};
    std::atomic<bool> stopped_ {false};
    std::atomic<unsigned> fast_ticks_ {0};
    // See Screen::games_apart().
    const bool games_apart_;

    mutable std::mutex peers_mutex_;
    std::map<std::uint64_t, Peer> peers_;
    // Ids we used before changing color: our own late packets from them must not look like another peer.
    std::vector<std::uint64_t> old_ids_;

    // Our pictures offered lately, kept for whoever downloads them or asks for their pieces.
    struct Outgoing {
        std::uint64_t sender = 0;
        std::uint64_t picture = 0;
        std::shared_ptr<const std::string> text;
        // When each piece was last sent in a Chunk.
        std::vector<clock::time_point> resent;
        clock::time_point sent;
    };
    std::mutex outgoing_mutex_;
    std::deque<Outgoing> outgoing_;
    std::atomic<std::uint64_t> next_picture_ {1};

    // Pictures being downloaded or arriving in Chunks, by sender and picture; only the chat thread uses them.
    struct Incoming {
        std::string name;
        std::vector<std::string> pieces;
        std::vector<bool> have;
        std::size_t received = 0;
        clock::time_point last_piece;
        clock::time_point last_request;
        clock::duration wait {};
        bool downloading = false;
    };
    std::map<IncomingKey, Incoming> incoming_;
    // The ones put together lately, so pieces sent again for somebody else do not start them over.
    std::deque<std::pair<std::uint64_t, std::uint64_t>> received_pictures_;

    // Downloads finished (the picture, or nullopt when it failed), for the chat thread.
    struct Download {
        IncomingKey key;
        std::optional<std::string> text;
    };
    std::mutex downloads_mutex_;
    std::vector<Download> downloads_;
    struct Transfer {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::mutex transfers_mutex_;
    std::vector<Transfer> transfers_;
    std::optional<net::TcpListener> listener_;

    // Avatars, by hash: ours, the ones downloaded, and the ones being downloaded or that failed lately.
    mutable std::mutex avatars_mutex_;
    std::uint64_t own_avatar_hash_ = 0;
    std::shared_ptr<const std::string> own_avatar_;
    std::map<std::uint64_t, std::shared_ptr<const std::string>> avatars_;
    std::set<std::uint64_t> avatars_fetching_;
    std::map<std::uint64_t, clock::time_point> avatars_failed_;
    std::jthread server_;
    // The files received, kept in files_dir_ (a temporary folder of this session's own, deleted when it ends).
    std::filesystem::path files_dir_;
    mutable std::mutex files_mutex_;
    std::vector<ReceivedFile> files_;
    // The trills coming or playing, for /stop to stop them.
    std::mutex trills_mutex_;
    std::vector<std::shared_ptr<std::stop_source>> trills_;

    // Channels: ours, what we know of the others' (by name), the current one, what was said in each, and those with
    // something new since they were shown.
    std::uint64_t user_ = 0;
    std::filesystem::path channels_file_;
    mutable std::mutex channels_mutex_;
    std::map<std::string, channel::Channel> channels_;
    std::string current_ {channel::general};
    std::map<std::string, std::deque<Entry>> backlog_;
    std::set<std::string> unread_;

    // Held while a hook runs, so set_game_hooks() can wait for it.
    std::mutex hooks_mutex_;
    GameHooks hooks_;

    std::jthread thread_;
};

} // namespace zchat
