#pragma once

#include "color.hpp"
#include "history.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "screen.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
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

    // Tells the others this peer is leaving and stops listening. Safe to call more than once.
    void stop();

    // The names of the other peers currently in the chat.
    std::vector<std::string> peers() const;

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

    // Prints messages from the history (see history.hpp), each with the time it was sent at.
    void print_history(const std::vector<history::Entry>& entries) const;

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
    };

    // Replaces the game hooks; empty ones are not called. Once it returns, the old ones are not running anymore.
    void set_game_hooks(GameHooks hooks);

    // Sends a Game packet to everybody (not to us).
    void send_game(std::string_view text);

private:
    using clock = std::chrono::steady_clock;

    struct Peer {
        std::string name;
        clock::time_point last_seen;
        std::deque<std::uint64_t> recent_seqs;
    };

    void run(std::stop_token stop);
    void handle(const Packet& packet);
    // With once, to each network once, see net::BroadcastSocket::broadcast().
    void send(PacketType type, std::string_view text = {}, bool once = false);
    // Returns whether the message tags us. when is the time it was sent at, for messages from the history.
    bool print_message(std::uint64_t id, std::string_view name, std::string_view text,
                       std::optional<std::time_t> when = std::nullopt) const;
    // Turns the "@Name" tags of the people in the chat into markup showing them bold in their color, ours also
    // underlined. Sets tags_us when we are tagged.
    std::string mark_mentions(std::string_view text, bool& tags_us) const;
    void print_art(std::uint64_t id, std::string_view name, std::string_view art) const;
    void print_picture(std::uint64_t id, std::string_view name, std::string_view text) const;
    void prune_silent_peers();
    // Pictures in Chunks: sending one piece, putting the pieces we receive together, asking again for the ones
    // that did not arrive, and sending again the ones asked for.
    void send_chunk(std::uint64_t picture, std::size_t index, std::size_t count, std::string_view piece);
    void receive_chunk(const Packet& packet);
    void request_missing_chunks();
    void resend_chunks(std::string_view request);

    // Changes with the color, see set_color().
    std::atomic<std::uint64_t> id_;
    mutable std::mutex name_mutex_;
    std::string name_;
    Screen& terminal_;
    net::BroadcastSocket socket_;
    std::atomic<std::uint64_t> seq_ {0};
    std::atomic<bool> stopped_ {false};

    mutable std::mutex peers_mutex_;
    std::map<std::uint64_t, Peer> peers_;
    // Ids we used before changing color: our own late packets from them must not look like another peer.
    std::vector<std::uint64_t> old_ids_;

    // Our pictures sent in Chunks lately, kept for whoever missed some of their pieces.
    struct Outgoing {
        std::uint64_t sender = 0;
        std::uint64_t picture = 0;
        std::vector<std::string> pieces;
        // When each piece was last sent again.
        std::vector<clock::time_point> resent;
        clock::time_point sent;
    };
    std::mutex outgoing_mutex_;
    std::deque<Outgoing> outgoing_;
    std::atomic<std::uint64_t> next_picture_ {1};

    // Pictures arriving in Chunks, by sender and picture; only the chat thread uses them.
    struct Incoming {
        std::string name;
        std::vector<std::string> pieces;
        std::vector<bool> have;
        std::size_t received = 0;
        clock::time_point last_piece;
        clock::time_point last_request;
        clock::duration wait {};
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, Incoming> incoming_;
    // The ones put together lately, so pieces sent again for somebody else do not start them over.
    std::deque<std::pair<std::uint64_t, std::uint64_t>> received_pictures_;

    // Held while a hook runs, so set_game_hooks() can wait for it.
    std::mutex hooks_mutex_;
    GameHooks hooks_;

    std::jthread thread_;
};

} // namespace zchat
