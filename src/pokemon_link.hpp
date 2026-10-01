#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// How a Pokémon battle travels between zchats, see src/pokemon.cpp: the pieces that know nothing of the chat.
namespace zchat::pokemon::link {

// A name as one field of a packet: '%' and ' ' escaped, and "%" for an empty one.
inline std::string encode_field(std::string_view s) {
    std::string out;
    for (const char c : s) {
        out += c == '%' ? std::string("%25") : c == ' ' ? std::string("%20") : std::string(1, c);
    }
    return out.empty() ? "%" : out;
}

inline std::string decode_field(std::string_view s) {
    if (s == "%") {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.substr(i, 3) == "%20") {
            out += ' ';
            i += 2;
        } else if (s.substr(i, 3) == "%25") {
            out += '%';
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The bridge's messages have their backslashes doubled and their line breaks as \n (see bridge.js).
inline std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out += s[i + 1] == 'n' ? '\n' : s[i + 1];
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// A message in parts of at most max bytes, never cutting a UTF-8 character (which would not get through: the chat
// replaces broken ones).
inline std::vector<std::string_view> split(std::string_view message, std::size_t max) {
    std::vector<std::string_view> parts;
    while (message.size() > max) {
        std::size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        if (cut == 0) {
            cut = max;
        }
        parts.push_back(message.substr(0, cut));
        message.remove_prefix(cut);
    }
    parts.push_back(message);
    return parts;
}

// The messages of a stream, numbered from 1, coming in parts, possibly out of order or twice: given back whole, in
// order. Parts of messages too far ahead, or that do not all come in time, are dropped (they are asked for again).
class Inbound {
public:
    using clock = std::chrono::steady_clock;

    explicit Inbound(clock::duration timeout = std::chrono::seconds(30)) :
        timeout_(timeout) {
    }

    // Part (from 0) of count of message seq: returns the messages it completes, in order.
    std::vector<std::string> add(std::uint64_t seq, std::size_t part, std::size_t count, std::string_view data) {
        std::vector<std::string> done;
        if (seq < next_ || seq >= next_ + max_ahead || count == 0 || count > max_parts || part >= count) {
            return done;
        }
        Partial& p = pending_[seq];
        if (p.parts.empty()) {
            p.parts.resize(count);
            p.have.assign(count, false);
            p.first = clock::now();
        }
        if (p.parts.size() != count || p.have[part]) {
            return done;
        }
        p.parts[part] = std::string(data);
        p.have[part] = true;
        ++p.received;
        for (auto it = pending_.find(next_); it != pending_.end() && it->second.received == it->second.parts.size();
             it = pending_.find(next_)) {
            std::string message;
            for (const auto& piece : it->second.parts) {
                message += piece;
            }
            done.push_back(std::move(message));
            pending_.erase(it);
            ++next_;
        }
        return done;
    }

    // How many messages came, in order.
    std::uint64_t received() const {
        return next_ - 1;
    }

    // How many messages are waiting for some of their parts.
    std::size_t partial() const {
        return pending_.size();
    }

    void drop_stale(clock::time_point now = clock::now()) {
        std::erase_if(pending_, [&](const auto& entry) {
            return now - entry.second.first > timeout_;
        });
    }

private:
    static constexpr std::uint64_t max_ahead = 256;
    static constexpr std::size_t max_parts = 64;

    struct Partial {
        std::vector<std::string> parts;
        std::vector<bool> have;
        std::size_t received = 0;
        clock::time_point first;
    };
    clock::duration timeout_;
    std::uint64_t next_ = 1;
    std::map<std::uint64_t, Partial> pending_;
};

} // namespace zchat::pokemon::link
