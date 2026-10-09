#include "chat.hpp"

#include "channel.hpp"
#include "cipher.hpp"
#include "file.hpp"
#include "image.hpp"
#include "markup.hpp"
#include "popup.hpp"
#include "sound.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace zchat {

namespace {

    using namespace std::chrono_literals;

    constexpr auto heartbeat_interval = 5s;
    constexpr auto peer_timeout = 16s;
    constexpr auto interfaces_refresh_interval = 30s;
    constexpr std::size_t dedup_window = 64;
    // "@everyone" tags all the people in the chat.
    constexpr std::string_view everyone_tag = "everyone";

    std::optional<std::uint64_t> parse_id(std::string_view s) {
        std::uint64_t id = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || id == 0) {
            return std::nullopt;
        }
        return id;
    }

    // Pictures too big for a packet, see Chat::send_picture(): at most this many downloads and uploads at once, and
    // how long they wait for the other end.
    constexpr std::size_t max_transfers = 16;
    constexpr auto connect_timeout = 3s;
    constexpr auto transfer_timeout = 15s;
    constexpr std::size_t max_outgoing_pictures = 4;
    // What is kept of each channel, to show it again when it is joined; pictures take room, so fewer of them.
    constexpr std::size_t max_backlog = 300;
    constexpr std::size_t max_backlog_pictures = 30;
    // How often the channels we are in are told about, for whoever missed a change (or just came).
    constexpr auto channels_interval = 20s;
    // Avatars kept, and how long before trying again to download one that failed.
    constexpr std::size_t max_avatars = 64;
    constexpr auto avatar_retry = 30s;

    // FNV-1a: tells avatars apart, so each is downloaded once.
    std::uint64_t avatar_hash(std::string_view s) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const char c : s) {
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        }
        return h ? h : 1;
    }
    constexpr auto outgoing_picture_lifetime = 2min;
    constexpr std::size_t max_incoming_pictures = 8;
    constexpr std::size_t max_received_pictures = 64;
    // How long after the last piece the missing ones are asked for (twice as long after each request that brings
    // nothing, up to chunk_max_wait); when a picture is given up on; how soon a piece is not sent again, for
    // everybody who asked for it at about the same time.
    constexpr auto chunk_wait = 300ms;
    constexpr auto chunk_max_wait = 2400ms;
    constexpr auto chunk_give_up = 20s;
    constexpr auto chunk_resend_interval = 250ms;
    // Trills: how long there is to catch the STOP button (or type /stop), and how long the picture flies around.
    constexpr std::chrono::milliseconds trill_countdown = 3s;
    constexpr std::chrono::milliseconds trill_picture_time = 3s;

    // "A B C ...": numbers, one space between them.
    std::optional<std::vector<std::uint64_t>> parse_numbers(std::string_view s) {
        std::vector<std::uint64_t> numbers;
        const char* p = s.data();
        const char* end = s.data() + s.size();
        while (p != end) {
            std::uint64_t n = 0;
            const auto [ptr, ec] = std::from_chars(p, end, n);
            if (ec != std::errc {} || (ptr != end && (*ptr != ' ' || ptr + 1 == end))) {
                return std::nullopt;
            }
            numbers.push_back(n);
            p = ptr == end ? end : ptr + 1;
        }
        return numbers;
    }

    std::tm local_time(std::time_t t) {
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        return local;
    }

    // The time of day, now or at t; with the date too when that is not today.
    std::string timestamp(std::time_t t = std::time(nullptr)) {
        const std::tm local = local_time(t);
        const std::tm today = local_time(std::time(nullptr));
        if (local.tm_year != today.tm_year || local.tm_yday != today.tm_yday) {
            return std::format("{}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                               local.tm_hour, local.tm_min);
        }
        return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    }

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    bool is_word_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

} // namespace

Chat::Chat(std::uint16_t port, std::string name, std::optional<Color> color, Screen& terminal) :
    id_(make_id(color)),
    name_(std::move(name)),
    terminal_(terminal),
    socket_(port),
    games_apart_(terminal.games_apart()) {
    // Without it, pictures too big for a packet can still be sent in Chunks.
    try {
        listener_.emplace();
    } catch (const std::exception&) {
    }
    std::error_code ec;
    files_dir_ = std::filesystem::temp_directory_path(ec) / "zchat-files" / std::format("{:x}", id_.load());
}

Chat::~Chat() {
    stop();
    std::error_code ec;
    std::filesystem::remove_all(files_dir_, ec);
}

void Chat::start() {
    thread_ = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
    if (listener_) {
        server_ = std::jthread([this](std::stop_token stop) {
            serve_pictures(stop);
        });
    }
    send(PacketType::Join);
    send_channel_states();
}

void Chat::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    // The thread first: what it sends after our Leave (a heartbeat, a game's packet) would bring us back.
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    server_ = {};
    std::vector<Transfer> transfers;
    {
        std::scoped_lock lock(transfers_mutex_);
        transfers.swap(transfers_);
    }
    // Their threads are asked to stop and joined.
    transfers.clear();
    send(PacketType::Leave);
}

void Chat::say(std::string_view text) {
    const std::string clean = text::sanitize(text, max_text_bytes);
    const std::string channel = speaking_channel();
    if (channel != channel::general) {
        if (!in_channel(channel)) {
            notice(std::format("You are not in {} anymore.", channel_label(channel)));
            switch_to(std::string(channel::general));
            return;
        }
        send(PacketType::ChannelMessage, std::format("{} m\n{}", channel, clean));
        deliver(channel, {Entry::Kind::Message, std::time(nullptr), id_, name(), std::make_shared<const std::string>(clean)});
        return;
    }
    say_in_general(clean, in_games_log());
}

void Chat::say_general(std::string_view text) {
    say_in_general(text::sanitize(text, max_text_bytes), false);
}

void Chat::say_in_general(const std::string& clean, bool from_games_log) {
    const std::string general(channel::general);
    send(PacketType::Message, clean);
    Entry entry {Entry::Kind::Message, std::time(nullptr), id_, name(), std::make_shared<const std::string>(clean)};
    if (from_games_log) {
        // Said from the games log: shown there if it is a move of a game (a race's words), and else in #general.
        bool move = false;
        {
            std::scoped_lock lock(hooks_mutex_);
            move = hooks_.claims && hooks_.claims(id_, clean);
        }
        if (move) {
            deliver(std::string(channel::games_log), std::move(entry));
        } else {
            deliver(general, std::move(entry));
            history::add({std::time(nullptr), id_, name(), clean});
            game_notice("Said in #general: the games log is for the games.");
        }
    } else {
        deliver(general, std::move(entry));
        history::add({std::time(nullptr), id_, name(), clean});
    }
    std::scoped_lock lock(hooks_mutex_);
    if (hooks_.message) {
        hooks_.message(id_, name(), clean);
    }
}

void Chat::set_game_hooks(GameHooks hooks) {
    std::scoped_lock lock(hooks_mutex_);
    hooks_ = std::move(hooks);
}

void Chat::send_game(std::string_view text) {
    send(PacketType::Game, text);
}

void Chat::draw(std::string_view art) {
    Packet packet;
    packet.type = PacketType::Art;
    packet.name = name();
    packet.text = std::string(art);
    // Cleaned up the same way the others will see it.
    const std::string clean = decode(encode(packet)).value_or(Packet {}).text;
    const std::string channel = speaking_channel();
    if (in_games_log()) {
        game_notice("Drawn in #general: the games log is for the games.");
    }
    if (channel == channel::general) {
        send(PacketType::Art, clean);
    } else {
        send(PacketType::ChannelMessage, std::format("{} a\n{}", channel, clean));
    }
    deliver(channel, {Entry::Kind::Art, std::time(nullptr), id_, name(), std::make_shared<const std::string>(clean)});
}

void Chat::send_picture(std::string_view picture) {
    Packet packet;
    packet.type = PacketType::Image;
    packet.name = name();
    // In a channel other than general, a line saying which comes first (older versions then ignore it).
    const std::string channel = speaking_channel();
    if (in_games_log()) {
        game_notice("Sent in #general: the games log is for the games.");
    }
    packet.text = channel == channel::general ? std::string(picture) : std::format("channel {}\n{}", channel, picture);
    const std::string clean = decode(encode(packet)).value_or(Packet {}).text;
    receive_picture(id_, name(), clean);
    if (clean.size() <= max_packet_image_bytes) {
        send(PacketType::Image, clean);
        return;
    }
    // Too big to broadcast well (Wi-Fi sends broadcasts slowly, and never again when they are lost): offered, and
    // downloaded by each of the others straight from us, see serve_picture(). Whoever cannot download it asks for
    // it in Chunks instead, see request_missing_chunks().
    Outgoing out {id_, next_picture_++, std::make_shared<const std::string>(clean), {}, clock::now()};
    const std::size_t count = (clean.size() + max_chunk_bytes - 1) / max_chunk_bytes;
    out.resent.resize(count);
    const std::uint64_t picture_id = out.picture;
    {
        std::scoped_lock lock(outgoing_mutex_);
        while (!outgoing_.empty() && (outgoing_.size() >= max_outgoing_pictures ||
                                      clock::now() - outgoing_.front().sent > outgoing_picture_lifetime)) {
            outgoing_.pop_front();
        }
        outgoing_.push_back(std::move(out));
    }
    send(PacketType::Offer,
         std::format("{} {} {} {}", picture_id, clean.size(), count, listener_ ? listener_->port() : 0));
}

void Chat::send_file(std::string_view file) {
    // Files and trills travel like pictures: receive_picture() tells them apart.
    send_picture(file);
}

std::optional<Chat::ReceivedFile> Chat::received_file(std::size_t index) const {
    std::scoped_lock lock(files_mutex_);
    if (index == 0 || index > files_.size()) {
        return std::nullopt;
    }
    return files_[index - 1];
}

std::size_t Chat::received_files() const {
    std::scoped_lock lock(files_mutex_);
    return files_.size();
}

void Chat::send_chunk(std::uint64_t picture, std::size_t index, std::size_t count, std::string_view piece) {
    std::string text = std::format("{} {} {}\n", picture, index, count);
    text += piece;
    send(PacketType::Chunk, text, true);
}

bool Chat::start_transfer(std::function<void(std::stop_token)> work) {
    std::scoped_lock lock(transfers_mutex_);
    std::erase_if(transfers_, [](const Transfer& t) {
        return t.done->load();
    });
    if (transfers_.size() >= max_transfers) {
        return false;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    transfers_.push_back({done, std::jthread([work = std::move(work), done](std::stop_token stop) {
                              work(stop);
                              *done = true;
                          })});
    return true;
}

void Chat::serve_pictures(std::stop_token stop) {
    while (!stop.stop_requested()) {
        auto stream = listener_->accept(200ms);
        if (!stream) {
            continue;
        }
        auto shared = std::make_shared<net::TcpStream>(std::move(*stream));
        start_transfer([this, shared](std::stop_token transfer_stop) {
            serve_picture(*shared, transfer_stop);
        });
    }
}

void Chat::serve_picture(net::TcpStream& stream, std::stop_token stop) {
    // "GET ID": the picture, scrambled like packets, and the connection closed. "AVATAR HASH": our avatar.
    const auto request = stream.read_line(64, 5s, stop);
    std::uint64_t picture = 0;
    if (request && request->starts_with("AVATAR ")) {
        std::shared_ptr<const std::string> avatar;
        {
            std::scoped_lock lock(avatars_mutex_);
            if (own_avatar_ && *request == std::format("AVATAR {:016x}", own_avatar_hash_)) {
                avatar = own_avatar_;
            }
        }
        if (avatar) {
            stream.send_all(cipher::scramble(*avatar), transfer_timeout, stop);
        }
        return;
    }
    if (!request || !request->starts_with("GET ")) {
        return;
    }
    const std::string_view number = std::string_view(*request).substr(4);
    if (const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), picture);
        ec != std::errc {} || ptr != number.data() + number.size()) {
        return;
    }
    std::shared_ptr<const std::string> text;
    {
        std::scoped_lock lock(outgoing_mutex_);
        const auto it = std::ranges::find_if(outgoing_, [&](const Outgoing& out) {
            return out.picture == picture;
        });
        if (it == outgoing_.end()) {
            return;
        }
        text = it->text;
    }
    stream.send_all(cipher::scramble(*text), transfer_timeout, stop);
}

void Chat::receive_offer(const Packet& packet, std::uint32_t from) {
    const auto numbers = parse_numbers(packet.text);
    if (!numbers || numbers->size() != 4) {
        return;
    }
    const std::uint64_t picture = (*numbers)[0];
    const std::uint64_t bytes = (*numbers)[1];
    const std::uint64_t count = (*numbers)[2];
    const std::uint64_t port = (*numbers)[3];
    if (bytes <= max_packet_image_bytes || bytes > max_image_bytes ||
        count != (bytes + max_chunk_bytes - 1) / max_chunk_bytes || port > 65535) {
        return;
    }
    const std::pair key {packet.sender, picture};
    if (std::ranges::find(received_pictures_, key) != received_pictures_.end() || incoming_.contains(key) ||
        incoming_.size() >= max_incoming_pictures) {
        return;
    }
    const auto c = static_cast<std::size_t>(count);
    Incoming& in = incoming_
                       .emplace(key, Incoming {packet.name, std::vector<std::string>(c), std::vector<bool>(c), 0,
                                               clock::now(), clock::now(), chunk_wait, false})
                       .first->second;
    in.downloading = port != 0 && from != 0 &&
                     start_transfer([this, key, from, port, bytes](std::stop_token stop) {
                         download_picture(key, from, static_cast<std::uint16_t>(port),
                                          static_cast<std::size_t>(bytes), stop);
                     });
    if (!in.downloading) {
        // Asked for in Chunks right away.
        in.last_piece = in.last_request = clock::now() - chunk_wait;
    }
}

void Chat::download_picture(std::pair<std::uint64_t, std::uint64_t> key, std::uint32_t from, std::uint16_t port,
                            std::size_t bytes, std::stop_token stop) {
    std::optional<std::string> text;
    if (auto stream = net::TcpStream::connect(from, port, connect_timeout);
        stream && stream->send_all(std::format("GET {}\n", key.second), connect_timeout, stop)) {
        // Scrambled: a few bytes more than the picture.
        if (const auto data = stream->read_all(bytes + 64, transfer_timeout, stop)) {
            text = cipher::unscramble(*data);
            if (text && text->size() != bytes) {
                text.reset();
            }
        }
    }
    std::scoped_lock lock(downloads_mutex_);
    downloads_.push_back({key, std::move(text)});
}

void Chat::finish_downloads() {
    std::vector<Download> done;
    {
        std::scoped_lock lock(downloads_mutex_);
        done.swap(downloads_);
    }
    for (auto& download : done) {
        const auto it = incoming_.find(download.key);
        if (it == incoming_.end()) {
            continue;
        }
        if (download.text) {
            finish_picture(it, *download.text);
        } else {
            // Could not be downloaded (e.g. a firewall): asked for in Chunks instead.
            it->second.downloading = false;
            it->second.last_piece = it->second.last_request = clock::now() - chunk_wait;
            it->second.wait = chunk_wait;
        }
    }
}

void Chat::finish_picture(std::map<std::pair<std::uint64_t, std::uint64_t>, Incoming>::iterator it,
                          std::string_view text) {
    const auto key = it->first;
    const std::string name = std::move(it->second.name);
    incoming_.erase(it);
    received_pictures_.push_back(key);
    if (received_pictures_.size() > max_received_pictures) {
        received_pictures_.pop_front();
    }
    receive_picture(key.first, name, text);
}

void Chat::receive_chunk(const Packet& packet) {
    const auto nl = packet.text.find('\n');
    if (nl == std::string::npos) {
        return;
    }
    const auto numbers = parse_numbers(std::string_view(packet.text).substr(0, nl));
    if (!numbers || numbers->size() != 3) {
        return;
    }
    const std::uint64_t picture = (*numbers)[0];
    const std::uint64_t index = (*numbers)[1];
    const std::uint64_t count = (*numbers)[2];
    if (count < 2 || count > max_chunks || index >= count) {
        return;
    }
    const std::pair key {packet.sender, picture};
    if (std::ranges::find(received_pictures_, key) != received_pictures_.end()) {
        return;
    }
    auto it = incoming_.find(key);
    if (it == incoming_.end()) {
        if (incoming_.size() >= max_incoming_pictures) {
            return;
        }
        const auto c = static_cast<std::size_t>(count);
        it = incoming_.emplace(key, Incoming {packet.name, std::vector<std::string>(c), std::vector<bool>(c), 0,
                                              clock::now(), clock::now(), chunk_wait, false})
                 .first;
    }
    Incoming& in = it->second;
    const auto i = static_cast<std::size_t>(index);
    if (in.pieces.size() != count || in.have[i]) {
        return;
    }
    in.pieces[i] = packet.text.substr(nl + 1);
    in.have[i] = true;
    in.last_piece = clock::now();
    in.wait = chunk_wait;
    if (++in.received < count) {
        return;
    }
    std::string text;
    for (const auto& piece : in.pieces) {
        text += piece;
    }
    finish_picture(it, text);
}

void Chat::request_missing_chunks() {
    const auto now = clock::now();
    for (auto it = incoming_.begin(); it != incoming_.end();) {
        Incoming& in = it->second;
        if (in.downloading) {
            ++it;
            continue;
        }
        if (now - in.last_piece > chunk_give_up) {
            notice(
                std::format("A picture or file from {} did not arrive whole.", colored_name(it->first.first, in.name)));
            it = incoming_.erase(it);
            continue;
        }
        // Once the pieces stop coming: the ones missing, as many as fit in a request.
        if (now - in.last_piece >= in.wait && now - in.last_request >= in.wait) {
            std::string request = std::format("{:x} {}", it->first.first, it->first.second);
            for (std::size_t i = 0; i < in.have.size() && request.size() + 12 < max_resend_bytes; ++i) {
                if (!in.have[i]) {
                    request += std::format(" {}", i);
                }
            }
            send(PacketType::Resend, request);
            in.last_request = now;
            in.wait = std::min<clock::duration>(in.wait * 2, chunk_max_wait);
        }
        ++it;
    }
}

void Chat::resend_chunks(std::string_view request) {
    const auto space = request.find(' ');
    if (space == std::string_view::npos) {
        return;
    }
    const auto sender = parse_id(request.substr(0, space));
    const auto numbers = parse_numbers(request.substr(space + 1));
    if (!sender || !numbers || numbers->empty()) {
        return;
    }
    const std::uint64_t picture = numbers->front();
    std::scoped_lock lock(outgoing_mutex_);
    const auto it = std::ranges::find_if(outgoing_, [&](const Outgoing& out) {
        return out.sender == *sender && out.picture == picture;
    });
    if (it == outgoing_.end()) {
        return;
    }
    const std::string_view text = *it->text;
    const std::size_t count = it->resent.size();
    const auto now = clock::now();
    for (std::size_t n = 1; n < numbers->size(); ++n) {
        const auto i = static_cast<std::size_t>((*numbers)[n]);
        if (i < count && now - it->resent[i] >= chunk_resend_interval) {
            it->resent[i] = now;
            send_chunk(picture, i, count, text.substr(i * max_chunk_bytes, max_chunk_bytes));
        }
    }
}

void Chat::set_avatar(std::optional<std::string> picture) {
    {
        std::scoped_lock lock(avatars_mutex_);
        if (picture) {
            own_avatar_hash_ = avatar_hash(*picture);
            own_avatar_ = std::make_shared<const std::string>(std::move(*picture));
            avatars_[own_avatar_hash_] = own_avatar_;
        } else {
            own_avatar_hash_ = 0;
            own_avatar_.reset();
        }
    }
    // The others see it on our next heartbeat: this one.
    send(PacketType::Here);
}

std::string Chat::own_avatar_hash() const {
    std::scoped_lock lock(avatars_mutex_);
    return own_avatar_hash_ ? std::format("{:016x}", own_avatar_hash_) : std::string();
}

std::string Chat::avatar_of(std::uint64_t id) const {
    if (id == id_) {
        return own_avatar_hash();
    }
    std::scoped_lock lock(peers_mutex_);
    const auto it = peers_.find(id);
    return it != peers_.end() && it->second.avatar ? std::format("{:016x}", it->second.avatar) : std::string();
}

std::optional<std::string> Chat::avatar(std::string_view hash) const {
    std::uint64_t h = 0;
    if (const auto [ptr, ec] = std::from_chars(hash.data(), hash.data() + hash.size(), h, 16);
        ec != std::errc {} || ptr != hash.data() + hash.size()) {
        return std::nullopt;
    }
    std::scoped_lock lock(avatars_mutex_);
    const auto it = avatars_.find(h);
    return it != avatars_.end() ? std::optional<std::string>(*it->second) : std::nullopt;
}

std::string Chat::presence_text() const {
    // "user USER", then "avatar HASH BYTES PORT" when we have one.
    std::string text = user_ ? std::format("user {:x}", user_) : std::string();
    std::scoped_lock lock(avatars_mutex_);
    if (own_avatar_ && listener_) {
        text += std::format("{}avatar {:016x} {} {}", text.empty() ? "" : " ", own_avatar_hash_, own_avatar_->size(),
                            listener_->port());
    }
    return text;
}

void Chat::receive_presence(std::uint64_t sender, std::string_view text, std::uint32_t from) {
    // "user USER" (from versions with channels), then "avatar HASH BYTES PORT", or nothing for no avatar.
    std::uint64_t user = 0;
    if (text.starts_with("user ")) {
        text.remove_prefix(5);
        const auto space = text.find(' ');
        const std::string_view hex = text.substr(0, space);
        if (const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), user, 16);
            ec != std::errc {} || ptr != hex.data() + hex.size()) {
            return;
        }
        text.remove_prefix(space == std::string_view::npos ? text.size() : space + 1);
    }
    std::uint64_t hash = 0;
    std::optional<std::vector<std::uint64_t>> numbers;
    if (text.starts_with("avatar ")) {
        text.remove_prefix(7);
        const auto space = text.find(' ');
        const std::string_view hex = text.substr(0, space);
        if (const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), hash, 16);
            ec != std::errc {} || ptr != hex.data() + hex.size() || space == std::string_view::npos) {
            return;
        }
        numbers = parse_numbers(text.substr(space + 1));
        if (!numbers || numbers->size() != 2 || (*numbers)[0] > max_avatar_bytes || (*numbers)[1] == 0 ||
            (*numbers)[1] > 65535) {
            return;
        }
    }
    {
        std::scoped_lock lock(peers_mutex_);
        if (const auto it = peers_.find(sender); it != peers_.end()) {
            it->second.avatar = hash;
            it->second.user = user;
            if (user) {
                names_[user] = it->second.name;
            }
        }
    }
    if (!hash || from == 0) {
        return;
    }
    {
        std::scoped_lock lock(avatars_mutex_);
        if (avatars_.contains(hash) || avatars_fetching_.contains(hash)) {
            return;
        }
        if (const auto failed = avatars_failed_.find(hash);
            failed != avatars_failed_.end() && clock::now() - failed->second < avatar_retry) {
            return;
        }
        avatars_fetching_.insert(hash);
    }
    const auto bytes = static_cast<std::size_t>((*numbers)[0]);
    const auto port = static_cast<std::uint16_t>((*numbers)[1]);
    if (!start_transfer([this, hash, from, port, bytes](std::stop_token stop) {
            download_avatar(hash, from, port, bytes, stop);
        })) {
        std::scoped_lock lock(avatars_mutex_);
        avatars_fetching_.erase(hash);
    }
}

void Chat::download_avatar(std::uint64_t hash, std::uint32_t from, std::uint16_t port, std::size_t bytes,
                           std::stop_token stop) {
    std::optional<std::string> text;
    if (auto stream = net::TcpStream::connect(from, port, connect_timeout);
        stream && stream->send_all(std::format("AVATAR {:016x}\n", hash), connect_timeout, stop)) {
        if (const auto data = stream->read_all(bytes + 64, transfer_timeout, stop)) {
            text = cipher::unscramble(*data);
        }
    }
    // Only a picture, and the one announced.
    if (text) {
        Packet packet;
        packet.type = PacketType::Image;
        packet.name = "avatar";
        packet.text = *text;
        const auto clean = decode(encode(packet));
        if (text->size() != bytes || avatar_hash(*text) != hash || !clean || clean->text != *text ||
            !image::parse_picture(*text)) {
            text.reset();
        }
    }
    std::scoped_lock lock(avatars_mutex_);
    avatars_fetching_.erase(hash);
    if (!text) {
        avatars_failed_[hash] = clock::now();
        return;
    }
    // A few kept, not every one ever seen.
    while (avatars_.size() >= max_avatars) {
        auto victim = avatars_.begin();
        if (victim->first == own_avatar_hash_) {
            ++victim;
        }
        if (victim == avatars_.end()) {
            break;
        }
        avatars_.erase(victim);
    }
    avatars_[hash] = std::make_shared<const std::string>(std::move(*text));
}

void Chat::set_user(std::uint64_t user, std::filesystem::path channels_file) {
    user_ = user;
    channels_file_ = std::move(channels_file);
    std::ifstream in(channels_file_);
    std::scoped_lock lock(channels_mutex_, peers_mutex_);
    for (std::string line; std::getline(in, line);) {
        // Besides the channels, "closed NAME" for a private chat closed, and "name USER NAME" for whom it is with.
        if (line.starts_with("closed ")) {
            closed_.insert(channel::clean_name(std::string_view(line).substr(7)));
        } else if (line.starts_with("name ")) {
            const std::string_view rest = std::string_view(line).substr(5);
            const auto space = rest.find(' ');
            std::uint64_t id = 0;
            if (space != std::string_view::npos) {
                const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + space, id, 16);
                if (ec == std::errc {} && ptr == rest.data() + space && id != 0 && space + 1 < rest.size()) {
                    names_[id] = text::sanitize(rest.substr(space + 1), max_name_bytes);
                }
            }
        } else if (auto c = channel::decode(line)) {
            channels_[c->name] = std::move(*c);
        }
    }
}

void Chat::save_channels() const {
    // (With channels_mutex_ held.)
    if (channels_file_.empty()) {
        return;
    }
    std::ofstream out(channels_file_, std::ios::trunc);
    std::set<std::uint64_t> partners;
    for (const auto& [name, c] : channels_) {
        out << channel::encode(c) << '\n';
        if (c.direct && c.has(user_)) {
            partners.insert(c.members.begin(), c.members.end());
            if (closed_.contains(name)) {
                out << "closed " << name << '\n';
            }
        }
    }
    partners.erase(user_);
    std::scoped_lock lock(peers_mutex_);
    for (const auto partner : partners) {
        if (const auto it = names_.find(partner); it != names_.end()) {
            out << std::format("name {:x} {}\n", partner, it->second);
        }
    }
}

bool Chat::in_channel(const std::string& name) const {
    if (name == channel::general) {
        return true;
    }
    std::scoped_lock lock(channels_mutex_);
    const auto it = channels_.find(name);
    return it != channels_.end() && !it->second.deleted && it->second.has(user_);
}

std::string Chat::current_channel() const {
    std::scoped_lock lock(channels_mutex_);
    return current_;
}

bool Chat::in_games_log() const {
    return games_apart_ && current_channel() == channel::games_log;
}

std::string Chat::speaking_channel() const {
    return in_games_log() ? std::string(channel::general) : current_channel();
}

std::string Chat::user_name(std::uint64_t user) const {
    if (user == user_) {
        return colored_own_name();
    }
    std::scoped_lock lock(peers_mutex_);
    for (const auto& [id, peer] : peers_) {
        if (peer.user == user) {
            return colored_name(id, peer.name);
        }
    }
    return "someone";
}

std::string Chat::plain_user_name(std::uint64_t user) const {
    if (user == user_) {
        return name();
    }
    std::scoped_lock lock(peers_mutex_);
    for (const auto& [id, peer] : peers_) {
        if (peer.user == user) {
            return peer.name;
        }
    }
    const auto it = names_.find(user);
    return it != names_.end() ? it->second : std::string();
}

std::uint64_t Chat::direct_partner(const std::string& channel) const {
    std::scoped_lock lock(channels_mutex_);
    const auto it = channels_.find(channel);
    if (it == channels_.end() || !it->second.direct || !it->second.has(user_)) {
        return 0;
    }
    for (const auto member : it->second.members) {
        if (member != user_) {
            return member;
        }
    }
    return 0;
}

std::string Chat::channel_label(const std::string& channel) const {
    const std::uint64_t partner = direct_partner(channel);
    if (!partner) {
        return "#" + channel;
    }
    std::string who = user_name(partner);
    if (who == "someone") {
        const std::string plain = plain_user_name(partner);
        who = plain.empty() ? who : plain;
    }
    return std::format("your private chat with {}", who);
}

std::string Chat::join_hint(const std::string& channel) const {
    const std::uint64_t partner = direct_partner(channel);
    const std::string who = partner ? plain_user_name(partner) : std::string();
    return who.empty() ? std::format("/join {}", channel) : std::format("/msg {}", who);
}

std::string Chat::direct_with(std::string_view person) const {
    while (!person.empty() && (person.front() == ' ' || person.front() == '@')) {
        person.remove_prefix(1);
    }
    const std::string wanted = lowercase(person);
    std::vector<std::pair<std::string, std::uint64_t>> chats;
    {
        std::scoped_lock lock(channels_mutex_);
        for (const auto& [name, c] : channels_) {
            if (c.direct && c.has(user_)) {
                for (const auto member : c.members) {
                    if (member != user_) {
                        chats.emplace_back(name, member);
                    }
                }
            }
        }
    }
    for (const auto& [name, partner] : chats) {
        if (!wanted.empty() && lowercase(plain_user_name(partner)) == wanted) {
            return name;
        }
    }
    return {};
}

std::optional<std::uint64_t> Chat::find_user(std::string_view person, std::string& error) const {
    while (!person.empty() && (person.front() == ' ' || person.front() == '@')) {
        person.remove_prefix(1);
    }
    const std::string wanted = lowercase(person);
    if (wanted.empty()) {
        error = "Say who, by the name they have in the chat.";
        return std::nullopt;
    }
    if (wanted == lowercase(name())) {
        return user_;
    }
    std::set<std::uint64_t> found;
    bool old_version = false;
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            if (lowercase(peer.name) != wanted) {
                continue;
            }
            if (peer.user) {
                found.insert(peer.user);
            } else {
                old_version = true;
            }
        }
    }
    if (found.size() == 1) {
        return *found.begin();
    }
    error = found.size() > 1 ? std::format("More than one person is called {}: ask one to change name.", person)
            : old_version ? std::format("{} has a zchat without channels: they need to /update.", person)
                          : std::format("Nobody called {} is in the chat (they must be online).", person);
    return std::nullopt;
}

std::optional<std::string> Chat::change_channel(const std::string& name,
                                                const std::function<std::optional<std::string>(channel::Channel&)>& edit) {
    std::optional<channel::Channel> changed;
    {
        std::scoped_lock lock(channels_mutex_);
        const auto it = channels_.find(name);
        // One not known yet has version 0, before its first.
        channel::Channel c = it != channels_.end() ? it->second : channel::Channel {name, 0};
        const std::uint64_t version = c.version;
        if (auto error = edit(c)) {
            return error;
        }
        c.version = version + 1;
        c.author = user_;
        if (c.members.size() > channel::max_members) {
            return std::format("#{} is full: at most {} people.", name, channel::max_members);
        }
        channels_[name] = c;
        save_channels();
        changed = std::move(c);
    }
    send(PacketType::ChannelState, channel::encode(*changed));
    return std::nullopt;
}

std::optional<std::string> Chat::create_channel(std::string_view name, bool is_public) {
    const std::string clean = channel::clean_name(name);
    if (clean.empty()) {
        return std::format("{} is not a channel name: use up to {} letters, digits, - and _.", name, channel::max_name);
    }
    if (clean == channel::general) {
        return "#general is always there.";
    }
    if (clean == channel::games_log) {
        return std::format("#{} is where a window shows what the games say: pick another name.", clean);
    }
    if (clean.starts_with(channel::direct_prefix)) {
        return std::format("Names starting with {} are for private chats (/msg NAME): pick another one.",
                           channel::direct_prefix);
    }
    const auto error = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
        if (c.version > 0 && !c.deleted) {
            return std::format("#{} already exists: /join {}", clean, clean);
        }
        c.is_public = is_public;
        c.deleted = false;
        c.owner = user_;
        c.members = {user_};
        return std::nullopt;
    });
    if (error) {
        return error;
    }
    switch_to(clean);
    return std::nullopt;
}

std::optional<std::string> Chat::join_channel(std::string_view name) {
    if (name.starts_with('@')) {
        return open_direct(name);
    }
    const std::string clean = channel::clean_name(name);
    if (clean.empty()) {
        return std::format("{} is not a channel name (see /channels).", name);
    }
    if (games_apart_ && clean == channel::games_log) {
        switch_to(clean);
        return std::nullopt;
    }
    if (clean.starts_with(channel::direct_prefix)) {
        if (!in_channel(clean)) {
            return "That is not a private chat of yours: /msg NAME opens one with NAME.";
        }
        std::scoped_lock lock(channels_mutex_);
        if (closed_.erase(clean)) {
            save_channels();
        }
    }
    if (clean != channel::general && !in_channel(clean)) {
        const auto error = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
            if (c.version == 0 || c.deleted) {
                return std::format("There is no #{} (see /channels, or /create {}).", clean, clean);
            }
            if (!c.is_public) {
                return std::format("#{} is private: someone in it must /add you.", clean);
            }
            c.members.insert(user_);
            return std::nullopt;
        });
        if (error) {
            return error;
        }
    }
    switch_to(clean);
    return std::nullopt;
}

std::optional<std::string> Chat::open_direct(std::string_view person) {
    // The one there is already, with them in the chat or not; or else a new one, with them in the chat.
    std::string name = direct_with(person);
    if (name.empty()) {
        std::string error;
        const auto user = find_user(person, error);
        if (!user) {
            return error;
        }
        if (*user == user_) {
            return "That is you: a private chat takes somebody else.";
        }
        name = channel::direct_name(user_, *user);
        if (!in_channel(name)) {
            if (const auto e = change_channel(name, [&](channel::Channel& c) -> std::optional<std::string> {
                    if (c.version > 0 && !c.direct) {
                        return "That private chat cannot be opened.";
                    }
                    c.is_public = false;
                    c.direct = true;
                    c.deleted = false;
                    c.owner = user_;
                    c.members = {user_, *user};
                    return std::nullopt;
                })) {
                return e;
            }
        }
    }
    {
        std::scoped_lock lock(channels_mutex_);
        if (closed_.erase(name)) {
            save_channels();
        }
    }
    switch_to(name);
    return std::nullopt;
}

std::optional<std::string> Chat::leave_channel(std::string_view name) {
    if (name.starts_with('@')) {
        const std::string direct = direct_with(name);
        if (direct.empty()) {
            return std::format("You have no private chat with {}.", name.substr(1));
        }
        return leave_channel(direct);
    }
    const std::string clean = name.empty() ? current_channel() : channel::clean_name(name);
    if (clean == channel::general) {
        return "Everybody is always in #general.";
    }
    if (games_apart_ && clean == channel::games_log) {
        return std::format("#{} is always there: it is where the games say what happens in them.", clean);
    }
    if (clean.empty() || !in_channel(clean)) {
        return std::format("You are not in #{}.", clean.empty() ? std::string(name) : clean);
    }
    // A private chat is only closed: off our list, until something new is said in it.
    if (direct_partner(clean)) {
        const std::string label = channel_label(clean);
        const std::string hint = join_hint(clean);
        {
            std::scoped_lock lock(channels_mutex_);
            closed_.insert(clean);
            unread_.erase(clean);
            save_channels();
        }
        notice(std::format("Closed {}: {} opens it again.", label, hint));
        if (current_channel() == clean) {
            switch_to(std::string(channel::general));
        }
        return std::nullopt;
    }
    if (const auto error = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
            c.members.erase(user_);
            return std::nullopt;
        })) {
        return error;
    }
    notice(std::format("You left #{}.", clean));
    if (current_channel() == clean) {
        switch_to(std::string(channel::general));
    }
    return std::nullopt;
}

std::optional<std::string> Chat::delete_channel(std::string_view name) {
    const std::string clean = channel::clean_name(name);
    if (clean == channel::general) {
        return "#general can never be removed.";
    }
    if (games_apart_ && clean == channel::games_log) {
        return std::format("#{} can never be removed: it is where the games say what happens in them.", clean);
    }
    if (clean.empty()) {
        return std::format("{} is not a channel name (see /channels).", name);
    }
    if (direct_partner(clean)) {
        return "A private chat cannot be deleted: /leave closes it.";
    }
    if (const auto error = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
            if (c.version == 0 || c.deleted) {
                return std::format("There is no #{}.", clean);
            }
            if (c.owner != user_) {
                return std::format("Only {}, who created #{}, can delete it.", user_name(c.owner), clean);
            }
            c.deleted = true;
            c.members.clear();
            return std::nullopt;
        })) {
        return error;
    }
    {
        std::scoped_lock lock(channels_mutex_);
        backlog_.erase(clean);
        unread_.erase(clean);
    }
    notice(std::format("Deleted #{}.", clean));
    if (current_channel() == clean) {
        switch_to(std::string(channel::general));
    }
    return std::nullopt;
}

std::optional<std::string> Chat::add_to_channel(std::string_view name, std::string_view person) {
    const std::string clean = name.empty() ? current_channel() : channel::clean_name(name);
    if (clean == channel::general) {
        return "Everybody is always in #general: /join another channel first, or name it (/add #CHANNEL NAME).";
    }
    if (games_apart_ && clean == channel::games_log) {
        return std::format("#{} is yours alone: what the games say, as your zchat hears it.", clean);
    }
    if (clean.empty() || !in_channel(clean)) {
        return std::format("You are not in #{}.", clean.empty() ? std::string(name) : clean);
    }
    if (direct_partner(clean)) {
        return "A private chat is just the two of you: /create NAME private for a channel with more people.";
    }
    std::string error;
    const auto user = find_user(person, error);
    if (!user) {
        return error;
    }
    if (const auto e = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
            if (c.has(*user)) {
                return std::format("{} is in #{} already.", user_name(*user), clean);
            }
            c.members.insert(*user);
            return std::nullopt;
        })) {
        return e;
    }
    notice(std::format("Added {} to #{}.", user_name(*user), clean));
    return std::nullopt;
}

std::optional<std::string> Chat::remove_from_channel(std::string_view name, std::string_view person) {
    const std::string clean = name.empty() ? current_channel() : channel::clean_name(name);
    if (clean == channel::general) {
        return "Nobody can be removed from #general.";
    }
    if (games_apart_ && clean == channel::games_log) {
        return std::format("#{} is yours alone: what the games say, as your zchat hears it.", clean);
    }
    if (clean.empty() || !in_channel(clean)) {
        return std::format("You are not in #{}.", clean.empty() ? std::string(name) : clean);
    }
    if (direct_partner(clean)) {
        return "A private chat is just the two of you: /leave closes it.";
    }
    std::string error;
    const auto user = find_user(person, error);
    if (!user) {
        return error;
    }
    if (*user == user_) {
        return leave_channel(clean);
    }
    if (const auto e = change_channel(clean, [&](channel::Channel& c) -> std::optional<std::string> {
            if (!c.has(*user)) {
                return std::format("{} is not in #{}.", user_name(*user), clean);
            }
            c.members.erase(*user);
            return std::nullopt;
        })) {
        return e;
    }
    notice(std::format("Removed {} from #{}.", user_name(*user), clean));
    return std::nullopt;
}

std::vector<Chat::ChannelInfo> Chat::channels() const {
    std::vector<ChannelInfo> out;
    std::scoped_lock lock(channels_mutex_);
    std::size_t everyone = 1;
    {
        std::scoped_lock peers_lock(peers_mutex_);
        everyone += peers_.size();
    }
    out.push_back({std::string(channel::general), true, true, current_ == channel::general,
                   unread_.contains(std::string(channel::general)), everyone});
    if (games_apart_) {
        out.push_back({std::string(channel::games_log), false, true, current_ == channel::games_log,
                       unread_.contains(std::string(channel::games_log)), 1});
    }
    std::vector<ChannelInfo> direct;
    for (const auto& [name, c] : channels_) {
        // Private ones only for their members; and none can be the games log (from versions without it).
        if (c.deleted || (!c.is_public && !c.has(user_)) || (games_apart_ && name == channel::games_log)) {
            continue;
        }
        if (!c.direct) {
            out.push_back({name, c.is_public, c.has(user_), current_ == name, unread_.contains(name),
                           c.members.size(), c.owner == user_});
            continue;
        }
        // Closed ones are not listed, unless shown.
        if (closed_.contains(name) && current_ != name) {
            continue;
        }
        ChannelInfo info {name, false, true, current_ == name, unread_.contains(name), 2, false, true, {}};
        const auto partner = *std::ranges::find_if(c.members, [&](std::uint64_t m) {
            return m != user_;
        });
        std::scoped_lock peers_lock(peers_mutex_);
        const auto online = std::ranges::find_if(peers_, [&](const auto& p) {
            return p.second.user == partner;
        });
        if (online != peers_.end()) {
            info.with = {online->second.name, ansi_foreground(color_of_id(online->first)),
                         online->second.avatar ? std::format("{:016x}", online->second.avatar) : std::string()};
        } else {
            const auto known = names_.find(partner);
            info.with.name = known != names_.end() ? known->second : "someone";
        }
        direct.push_back(std::move(info));
    }
    std::ranges::sort(direct, {}, [](const ChannelInfo& c) {
        return lowercase(c.with.name);
    });
    out.insert(out.end(), std::make_move_iterator(direct.begin()), std::make_move_iterator(direct.end()));
    return out;
}

std::vector<std::string> Chat::channel_members(std::string_view name, std::size_t& away) const {
    away = 0;
    const std::string clean = name.empty() ? current_channel() : channel::clean_name(name);
    std::set<std::uint64_t> members;
    if (clean == channel::general) {
        std::vector<std::string> names = peers();
        names.push_back(colored_own_name());
        return names;
    }
    if (games_apart_ && clean == channel::games_log) {
        return {colored_own_name()};
    }
    {
        std::scoped_lock lock(channels_mutex_);
        const auto it = channels_.find(clean);
        if (it == channels_.end() || it->second.deleted || (!it->second.is_public && !it->second.has(user_))) {
            return {};
        }
        members = it->second.members;
    }
    std::vector<std::string> names;
    for (const auto member : members) {
        const std::string n = user_name(member);
        if (n == "someone") {
            ++away;
        } else {
            names.push_back(n);
        }
    }
    return names;
}

Chat::ChannelPeople Chat::channel_people(std::string_view channel_name) const {
    ChannelPeople people;
    const std::string clean = channel_name.empty() ? current_channel() : channel::clean_name(channel_name);
    const bool general = clean == channel::general;
    std::set<std::uint64_t> members;
    if (!general) {
        std::scoped_lock lock(channels_mutex_);
        const auto it = channels_.find(clean);
        if (it == channels_.end() || it->second.deleted || (!it->second.is_public && !it->second.has(user_))) {
            return people;
        }
        members = it->second.members;
    }
    const auto own_hash = own_avatar_hash();
    if (general || members.contains(user_)) {
        people.members.push_back({name(), ansi_foreground(color()), own_hash});
    }
    std::set<std::uint64_t> seen {user_};
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            Screen::Mention m {peer.name, ansi_foreground(color_of_id(id)),
                               peer.avatar ? std::format("{:016x}", peer.avatar) : std::string()};
            if (general || members.contains(peer.user)) {
                if (general || seen.insert(peer.user).second) {
                    people.members.push_back(std::move(m));
                }
            } else if (peer.user && seen.insert(peer.user).second) {
                // (Versions without channels cannot be added.)
                people.others.push_back(std::move(m));
            }
        }
    }
    if (!general) {
        std::size_t here = 0;
        for (const auto member : members) {
            here += seen.contains(member) ? 1 : 0;
        }
        people.away = members.size() - here;
    }
    const auto by_name = [](const Screen::Mention& m) {
        return lowercase(m.name);
    };
    std::ranges::sort(people.members, {}, by_name);
    std::ranges::sort(people.others, {}, by_name);
    return people;
}

void Chat::switch_to(const std::string& name) {
    std::vector<Entry> entries;
    std::string what;
    bool direct = false;
    {
        std::scoped_lock lock(channels_mutex_);
        current_ = name;
        unread_.erase(name);
        if (const auto it = backlog_.find(name); it != backlog_.end()) {
            entries.assign(it->second.begin(), it->second.end());
        }
        if (const auto it = channels_.find(name); it != channels_.end()) {
            direct = it->second.direct;
            what = std::format(" ({}, {} {})", it->second.is_public ? "public" : "private", it->second.members.size(),
                               it->second.members.size() == 1 ? "person" : "people");
        }
    }
    // A window shows only the channel; a terminal goes on below a line.
    if (!terminal_.clear()) {
        const std::string title = direct ? "@" + plain_user_name(direct_partner(name)) : "#" + name;
        terminal_.print(terminal_.colors() ? std::format("\x1b[90m──────── {} ────────\x1b[0m", title)
                                           : std::format("-------- {} --------", title));
    }
    if (direct) {
        notice(std::format("You are in {}: only the two of you see what is said, drawn and sent here; /leave "
                           "closes it.{}",
                           channel_label(name),
                           entries.empty() ? " Nothing said here yet (since zchat started)." : ""));
        for (const auto& entry : entries) {
            show(entry, false);
        }
        return;
    }
    if (games_apart_ && name == channel::games_log) {
        notice(std::format("You are in #{}: what the games say is here, so the chat stays a chat. What you type "
                           "here is said in #general (but a race's words, shown here).{}",
                           name, entries.empty() ? " No game has said anything yet (since zchat started)." : ""));
    } else {
        notice(std::format("You are in #{}{}.{}", name, what,
                           entries.empty() ? " Nothing said here yet (since zchat started)." : ""));
    }
    for (const auto& entry : entries) {
        show(entry, false);
    }
}

void Chat::receive_channel_state(const Packet& packet) {
    auto incoming = channel::decode(packet.text);
    if (!incoming) {
        return;
    }
    const channel::Channel c = std::move(*incoming);
    bool was_member = false;
    bool is_new = false;
    {
        std::scoped_lock lock(channels_mutex_);
        const auto it = channels_.find(c.name);
        const std::optional<channel::Channel> known =
            it != channels_.end() ? std::optional<channel::Channel>(it->second) : std::nullopt;
        if (!channel::accepts(known, c)) {
            return;
        }
        is_new = !known || known->deleted;
        was_member = known && !known->deleted && known->has(user_);
        channels_[c.name] = c;
        save_channels();
    }
    const bool now_member = !c.deleted && c.has(user_);
    // A private chat says nothing until something is said in it (see deliver()).
    if (c.author == user_ || c.direct) {
        return;
    }
    const std::string author = user_name(c.author);
    if (!was_member && now_member) {
        notice(std::format("{} added you to #{}: /join {}", author, c.name, c.name));
    } else if (was_member && !now_member) {
        notice(c.deleted ? std::format("{} deleted #{}.", author, c.name)
                         : std::format("{} removed you from #{}.", author, c.name));
        if (current_channel() == c.name) {
            switch_to(std::string(channel::general));
        }
    } else if (is_new && c.is_public && !c.deleted && c.version == 1) {
        notice(std::format("{} created #{}: /join {}", author, c.name, c.name));
    }
}

void Chat::send_channel_states() {
    std::vector<std::string> states;
    {
        std::scoped_lock lock(channels_mutex_);
        for (const auto& [name, c] : channels_) {
            // What we are in, and what is gone (so it stays gone); the others' own members tell about the rest.
            if (c.deleted || c.has(user_)) {
                states.push_back(channel::encode(c));
            }
        }
    }
    for (const auto& state : states) {
        send(PacketType::ChannelState, state);
    }
}

void Chat::set_name(std::string name) {
    {
        std::scoped_lock lock(name_mutex_);
        name_ = std::move(name);
    }
    // Peers notice the new name on any packet from us; a heartbeat now saves waiting for the next one.
    send(PacketType::Here);
}

void Chat::set_color(Color color) {
    const std::uint64_t old_id = id_;
    if (color_of_id(old_id) == color) {
        return;
    }
    const std::uint64_t new_id = make_id(color);
    {
        std::scoped_lock lock(peers_mutex_);
        old_ids_.push_back(old_id);
    }
    // The color comes from the id, so we become a new peer. The Leave of the old id names the new one: newer
    // zchat versions just move us over, older ones see us leave and be back right away.
    send(PacketType::Leave, std::format("{:x}", new_id));
    id_ = new_id;
    send(PacketType::Here);
}

std::string Chat::paint(Color color, std::string_view text) const {
    if (!terminal_.colors()) {
        return std::string(text);
    }
    return std::format("\x1b[{}m{}\x1b[0m", ansi_foreground(color), text);
}

void Chat::print_history(const std::vector<history::Entry>& entries) {
    for (const auto& entry : entries) {
        // Tags are shown, but ring no bell: they were heard back then.
        print_message(entry.sender, entry.name, entry.text, entry.time);
    }
    // Shown again when general is joined again.
    std::scoped_lock lock(channels_mutex_);
    auto& backlog = backlog_[std::string(channel::general)];
    for (const auto& entry : entries) {
        backlog.push_back({Entry::Kind::Message, entry.time, entry.sender, entry.name,
                           std::make_shared<const std::string>(entry.text)});
    }
}

bool Chat::print_message(std::uint64_t id, std::string_view name, std::string_view text,
                         std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    bool tags_us = false;
    const auto quote = markup::split_quote(text);
    const std::string_view said = quote ? quote->reply : text;
    const std::string marked = mark_mentions(said, tags_us);
    const std::string line =
        std::format("{} {}: {}", time, colored_name(id, name), markup::render(marked, terminal_.colors()));
    const std::string reply_quote = markup::quote(name, text);
    if (!quote) {
        if (!terminal_.show_message(line, std::nullopt, reply_quote)) {
            terminal_.print(line);
        }
        return tags_us;
    }
    // Being quoted tags us too.
    tags_us = tags_us || quotes_us(*quote);
    const std::string quoted =
        std::format("{}: {}", colored_name_of(quote->name), markup::render(quote->text, false));
    if (!terminal_.show_message(line, quoted, reply_quote)) {
        // The quote first, under the time, then the message.
        terminal_.print(terminal_.colors() ? std::format("\x1b[90m      ┃\x1b[0m {}\n{}", quoted, line)
                                           : std::format("      | {}\n{}", quoted, line));
    }
    return tags_us;
}

bool Chat::quotes_us(const markup::Quote& quote) const {
    return lowercase(quote.name) == lowercase(name());
}

std::string Chat::colored_name_of(std::string_view name) const {
    const std::string wanted = lowercase(name);
    if (wanted == lowercase(this->name())) {
        return colored_name(id_, name);
    }
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            if (lowercase(peer.name) == wanted) {
                return colored_name(id, name);
            }
        }
    }
    // Gone, or renamed since.
    return std::string(name);
}

std::optional<std::string> Chat::quote_last(std::string_view person) const {
    const std::string wanted = lowercase(person);
    std::scoped_lock lock(channels_mutex_);
    const auto it = backlog_.find(current_);
    if (it == backlog_.end()) {
        return std::nullopt;
    }
    for (const auto& entry : std::views::reverse(it->second)) {
        if (entry.kind != Entry::Kind::Message || !entry.data) {
            continue;
        }
        if (wanted.empty() ? entry.id != id_ : lowercase(entry.name) == wanted) {
            return markup::quote(entry.name, *entry.data);
        }
    }
    return std::nullopt;
}

std::string Chat::mark_mentions(std::string_view text, bool& tags_us) const {
    struct Person {
        std::string name;
        Color color;
        bool us;
        bool everyone = false;
    };
    std::vector<Person> people;
    // "@everyone" tags us too. First, so it wins over someone named "everyone": that tags them anyway.
    people.push_back({std::string(everyone_tag), {}, true, true});
    const std::uint64_t own_id = id_;
    people.push_back({lowercase(name()), color_of_id(own_id), true});
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            people.push_back({lowercase(peer.name), color_of_id(id), false});
        }
    }
    // A name with '<' or '>' would break the markup around it.
    std::erase_if(people, [](const Person& p) {
        return p.name.empty() || p.name.find_first_of("<>") != std::string::npos;
    });
    // The longest name first, so "@Rex Jr" is not taken for "@Rex".
    std::ranges::stable_sort(people, std::ranges::greater {}, [](const Person& p) {
        return p.name.size();
    });

    const std::string lower = lowercase(text);
    std::string out;
    std::size_t done = 0;
    for (std::size_t at = lower.find('@'); at != std::string::npos; at = lower.find('@', at + 1)) {
        // Only an '@' starting a word, not one in an email address.
        if (at > 0 && is_word_char(lower[at - 1])) {
            continue;
        }
        const auto person = std::ranges::find_if(people, [&](const Person& p) {
            const std::size_t end = at + 1 + p.name.size();
            return lower.compare(at + 1, p.name.size(), p.name) == 0 &&
                   (end >= lower.size() || !is_word_char(lower[end]));
        });
        if (person == people.end()) {
            continue;
        }
        const std::size_t end = at + 1 + person->name.size();
        const Color c = person->color;
        out += text.substr(done, at - done);
        if (person->everyone) {
            // Nobody's color: just bold and underlined, as it tags whoever reads it.
            out += std::format("<b><u>{}</u></b>", text.substr(at, end - at));
        } else {
            out += std::format("<b>{}<color=#{:02x}{:02x}{:02x}>{}</color>{}</b>", person->us ? "<u>" : "", c.r, c.g,
                               c.b, text.substr(at, end - at), person->us ? "</u>" : "");
        }
        tags_us = tags_us || person->us;
        done = end;
        at = end - 1;
    }
    out += text.substr(done);
    return out;
}

void Chat::print_art(std::uint64_t id, std::string_view name, std::string_view art, std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    // All in one print, so lines printed meanwhile by other threads do not end up in the middle of the drawing.
    std::string out = std::format("{} {}:", time, colored_name(id, name));
    const std::string drawing = image::render(art, terminal_.colors());
    for (std::string_view rest = drawing; !rest.empty();) {
        const auto nl = rest.find('\n');
        out += "\n      ";
        out += rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    }
    terminal_.print(out);
}

void Chat::receive_picture(std::uint64_t id, std::string_view name, std::string_view text) {
    // In a channel other than general: "channel NAME" first.
    std::string channel(channel::general);
    if (text.starts_with("channel ")) {
        const auto nl = text.find('\n');
        if (nl == std::string_view::npos) {
            return;
        }
        channel = channel::clean_name(text.substr(8, nl - 8));
        text.remove_prefix(nl + 1);
        if (channel.empty() || !in_channel(channel)) {
            return;
        }
    }
    Entry entry {Entry::Kind::Picture, std::time(nullptr), id, std::string(name), nullptr};
    if (file::is_trill(text)) {
        // As it arrives, in whatever channel is shown: all its members get it at once.
        if (!receive_trill(id, name, text, entry)) {
            return;
        }
    } else if (file::is_file(text)) {
        if (!keep_file(id, name, text, entry)) {
            return;
        }
    } else {
        if (!image::parse_picture(text)) {
            return;
        }
        entry.data = std::make_shared<const std::string>(text);
    }
    deliver(channel, std::move(entry));
}

void Chat::print_picture(std::uint64_t id, std::string_view name, std::string_view text,
                         std::optional<std::time_t> when) const {
    const auto picture = image::parse_picture(text);
    if (!picture) {
        return;
    }
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    std::vector<Screen::Frame> frames;
    for (const auto& frame : picture->frames) {
        frames.push_back({frame.mime, frame.base64, frame.delay_ms});
    }
    if (terminal_.show_image(std::format("{} {}:", time, colored_name(id, name)), picture->width, picture->height,
                             frames)) {
        return;
    }
    // Where pictures cannot be shown, drawn with characters: about one per 8 pixels, as wide as a terminal allows.
    const std::size_t cols = std::clamp<std::size_t>(static_cast<std::size_t>(picture->width) / 8, 8, max_art_cols);
    // (An animation: its first frame.)
    if (const auto art = image::to_ascii_data(picture->first, cols, max_art_rows)) {
        print_art(id, name, *art, when);
    }
}

bool Chat::keep_file(std::uint64_t id, std::string_view name, std::string_view text, Entry& entry) {
    const auto received = file::parse(text);
    if (!received) {
        return false;
    }
    std::size_t index = 0;
    {
        std::scoped_lock lock(files_mutex_);
        // A folder for each, so it keeps its name even when another has the same.
        const auto dir = files_dir_ / std::to_string(files_.size() + 1);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / std::filesystem::path(std::u8string(received->name.begin(), received->name.end()));
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(received->data.data(), static_cast<std::streamsize>(received->data.size()));
        if (out.flush()) {
            files_.push_back({path, received->name});
            index = files_.size();
        }
    }
    if (index == 0) {
        notice(std::format("A file from {} could not be kept: {}", colored_name(id, name), received->name));
        return false;
    }
    entry.kind = Entry::Kind::File;
    entry.file_index = index;
    entry.file_name = received->name;
    entry.file_size = file::format_size(received->data.size());
    return true;
}

void Chat::print_file(const Entry& entry, std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    const std::string who = colored_name(entry.id, entry.name);
    if (terminal_.show_file(std::format("{} {}:", time, who), entry.file_index, entry.file_name, entry.file_size)) {
        return;
    }
    const std::string hint = std::format("/save {} to download", entry.file_index);
    terminal_.print(std::format("{} {}: file {} ({}), {}", time, who, entry.file_name, entry.file_size,
                                terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", hint) : hint));
}

bool Chat::receive_trill(std::uint64_t id, std::string_view name, std::string_view text, Entry& entry) {
    auto trill = file::parse_trill(text);
    if (!trill) {
        return false;
    }
    entry.kind = Entry::Kind::Trill;
    entry.file_name = trill->sound.empty()     ? trill->picture_name
                      : trill->picture.empty() ? trill->sound_name
                                               : std::format("{} + {}", trill->sound_name, trill->picture_name);
    // Ours: only the others get it.
    if (id == id_) {
        return true;
    }
    auto shared = std::make_shared<const file::Trill>(std::move(*trill));
    // (With too many transfers and trills at once, this one is missed.)
    start_transfer([this, id, who = std::string(name), shared](std::stop_token stop) {
        run_trill(id, who, *shared, stop);
    });
    return true;
}

void Chat::run_trill(std::uint64_t id, const std::string& name, const file::Trill& trill, std::stop_token stop) {
    // Stopped by /stop, or when zchat quits.
    const auto cancel = std::make_shared<std::stop_source>();
    const std::stop_callback quit(stop, [cancel] {
        cancel->request_stop();
    });
    {
        std::scoped_lock lock(trills_mutex_);
        trills_.push_back(cancel);
    }
    const std::stop_token token = cancel->get_token();
    // Ready to go as soon as the countdown is over.
    const auto sound = trill.sound.empty() ? std::filesystem::path() : trill_file(trill.sound_name, trill.sound);
    const auto picture = trill.picture.empty() ? std::nullopt : image::parse_picture(trill.picture);
    bool caught = false;
    if (popup::available()) {
        caught = popup::alert(std::format("{} is trilling you!", name), trill_countdown, token);
    } else {
        notice(std::format("🔊 {} is trilling you in {} seconds: /stop to stop it!", colored_name(id, name),
                           std::chrono::duration_cast<std::chrono::seconds>(trill_countdown).count()));
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, token, trill_countdown, [] {
            return false;
        });
    }
    if (caught) {
        notice(std::format("You stopped {}'s trill in time!", colored_name(id, name)));
    } else if (!token.stop_requested()) {
        // All at once; each thread is waited for at the end of the block.
        const std::jthread nudging([this] {
            terminal_.nudge();
        });
        std::jthread playing;
        if (!sound.empty()) {
            playing = std::jthread([&sound, &token] {
                sound::play(sound, token);
            });
        }
        if (picture && popup::available()) {
            if (const auto decoded = image::decode_image(picture->first)) {
                popup::fly(*decoded, trill_picture_time, token);
            }
        } else if (picture) {
            // Without popups, in the chat.
            print_picture(id, name, trill.picture);
        }
    }
    std::scoped_lock lock(trills_mutex_);
    std::erase(trills_, cancel);
}

std::filesystem::path Chat::trill_file(std::string_view name, std::string_view data) {
    // By name only: a trill sent again (Fahhh!) plays the copy saved the first time.
    std::scoped_lock lock(files_mutex_);
    const auto path = files_dir_ / "trills" / std::filesystem::path(std::u8string(name.begin(), name.end()));
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        return path;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.write(data.data(), static_cast<std::streamsize>(data.size())).flush()) {
        out.close();
        std::filesystem::remove(path, ec);
        return {};
    }
    return path;
}

std::size_t Chat::stop_trills() {
    std::scoped_lock lock(trills_mutex_);
    std::size_t stopped = 0;
    for (const auto& trill : trills_) {
        stopped += trill->request_stop() ? 1 : 0;
    }
    return stopped;
}

void Chat::print_trill(const Entry& entry, std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    terminal_.print(std::format("{} {}: 🔊 trill! {}", time, colored_name(entry.id, entry.name), entry.file_name));
}

void Chat::show(const Entry& entry, bool live) {
    const std::optional<std::time_t> when = live ? std::nullopt : std::optional<std::time_t>(entry.time);
    switch (entry.kind) {
    case Entry::Kind::Message:
        // Tags ring the bell when they are heard, not when shown again.
        if (print_message(entry.id, entry.name, *entry.data, when) && live && entry.id != id_) {
            terminal_.bell();
        }
        break;
    case Entry::Kind::Art:
        print_art(entry.id, entry.name, *entry.data, when);
        break;
    case Entry::Kind::Picture:
        print_picture(entry.id, entry.name, *entry.data, when);
        break;
    case Entry::Kind::File:
        print_file(entry, when);
        break;
    case Entry::Kind::Trill:
        print_trill(entry, when);
        break;
    case Entry::Kind::Line:
        terminal_.print(*entry.data);
        break;
    }
}

void Chat::deliver(const std::string& channel, Entry entry) {
    bool current = false;
    bool first_unread = false;
    {
        std::scoped_lock lock(channels_mutex_);
        // Something new in a private chat closed opens it again.
        if (closed_.erase(channel)) {
            save_channels();
        }
        current = channel == current_;
        if (!current) {
            first_unread = unread_.insert(channel).second;
        }
        auto& backlog = backlog_[channel];
        backlog.push_back(entry);
        while (backlog.size() > max_backlog) {
            backlog.pop_front();
        }
        // Pictures take room: only the last ones are kept.
        const auto is_picture = [](const Entry& e) {
            return e.kind == Entry::Kind::Picture;
        };
        if (std::ranges::count_if(backlog, is_picture) > static_cast<std::ptrdiff_t>(max_backlog_pictures)) {
            backlog.erase(std::ranges::find_if(backlog, is_picture));
        }
    }
    if (current) {
        show(entry, true);
        return;
    }
    // Elsewhere: said once until it is visited, and tags always. The games log only gets its dot in the list.
    if (games_apart_ && channel == channel::games_log) {
        return;
    }
    bool tags_us = false;
    if (entry.kind == Entry::Kind::Message && entry.id != id_) {
        const auto quote = markup::split_quote(*entry.data);
        mark_mentions(quote ? quote->reply : std::string_view(*entry.data), tags_us);
        tags_us = tags_us || (quote && quotes_us(*quote));
    }
    // A private message rings like a tag.
    if (direct_partner(channel) && entry.id != id_) {
        terminal_.bell();
        if (first_unread || tags_us) {
            notice(std::format("{} sent you a private message: {}", colored_name(entry.id, entry.name),
                               join_hint(channel)));
        }
    } else if (tags_us) {
        terminal_.bell();
        notice(std::format("{} tagged you in #{}: /join {}", colored_name(entry.id, entry.name), channel, channel));
    } else if (first_unread) {
        notice(std::format("New messages in #{}: /join {}", channel, channel));
    }
}

std::vector<std::string> Chat::peers() const {
    std::vector<std::string> names;
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            names.push_back(colored_name(id, peer.name));
        }
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::pair<std::uint64_t, std::string>> Chat::people() const {
    std::vector<std::pair<std::uint64_t, std::string>> people;
    std::scoped_lock lock(peers_mutex_);
    for (const auto& [id, peer] : peers_) {
        people.emplace_back(id, peer.name);
    }
    return people;
}

std::vector<Screen::Mention> Chat::mentionable() const {
    std::vector<Screen::Mention> people;
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            people.push_back({peer.name, terminal_.colors() ? ansi_foreground(color_of_id(id)) : std::string(),
                              peer.avatar ? std::format("{:016x}", peer.avatar) : std::string()});
        }
    }
    std::ranges::sort(people, {}, [](const Screen::Mention& m) {
        return lowercase(m.name);
    });
    // Two peers with the same name are tagged the same way.
    const auto dupes = std::ranges::unique(people, {}, [](const Screen::Mention& m) {
        return lowercase(m.name);
    });
    people.erase(dupes.begin(), dupes.end());
    // Tagging everyone is offered first, when there is somebody to tag, and only once if someone is named so.
    std::erase_if(people, [](const Screen::Mention& m) {
        return lowercase(m.name) == everyone_tag;
    });
    if (!people.empty()) {
        people.insert(people.begin(), {std::string(everyone_tag), terminal_.colors() ? "1" : ""});
    }
    return people;
}

std::string Chat::colored_name(std::uint64_t id, std::string_view name) const {
    if (!terminal_.colors()) {
        return std::string(name);
    }
    // Our own name is bold, so it stands out.
    return std::format("\x1b[{}{}m{}\x1b[0m", id == id_ ? "1;" : "", ansi_foreground(color_of_id(id)), name);
}

std::string Chat::notice_line(std::string_view text) const {
    return terminal_.colors() ? std::format("\x1b[90m{} *\x1b[0m {}", timestamp(), text)
                              : std::format("{} * {}", timestamp(), text);
}

void Chat::notice(std::string_view text) const {
    terminal_.print(notice_line(text));
}

void Chat::game_notice(std::string_view text) {
    if (!games_apart_) {
        notice(text);
        return;
    }
    deliver(std::string(channel::games_log), {Entry::Kind::Line, std::time(nullptr), id_, name(),
                                              std::make_shared<const std::string>(notice_line(text))});
}

void Chat::game_print(std::string_view line) {
    if (!games_apart_) {
        terminal_.print(line);
        return;
    }
    deliver(std::string(channel::games_log),
            {Entry::Kind::Line, std::time(nullptr), id_, name(), std::make_shared<const std::string>(line)});
}

void Chat::send(PacketType type, std::string_view text, bool once) {
    Packet packet;
    packet.type = type;
    packet.sender = id_;
    packet.seq = ++seq_;
    packet.name = name();
    // Heartbeats tell about our avatar.
    packet.text = text.empty() && (type == PacketType::Here || type == PacketType::Join) ? presence_text()
                                                                                        : std::string(text);
    socket_.broadcast(cipher::scramble(encode(packet)), once);
}

void Chat::run(std::stop_token stop) {
    auto next_heartbeat = clock::now() + heartbeat_interval;
    auto next_refresh = clock::now() + interfaces_refresh_interval;
    auto next_channels = clock::now() + channels_interval;
    while (!stop.stop_requested()) {
        if (auto datagram = socket_.receive(fast_ticks_ != 0 ? 20ms : 200ms)) {
            // Older versions send plaintext: still understood, but never sent.
            const auto plain = cipher::unscramble(datagram->data);
            if (auto packet = decode(plain ? *plain : datagram->data); packet && packet->sender != id_) {
                handle(*packet, datagram->from);
            }
        }
        finish_downloads();
        request_missing_chunks();
        const auto now = clock::now();
        if (now >= next_refresh) {
            socket_.refresh_targets();
            next_refresh = now + interfaces_refresh_interval;
        }
        if (now >= next_channels) {
            send_channel_states();
            next_channels = now + channels_interval;
        }
        if (now >= next_heartbeat) {
            send(PacketType::Here);
            prune_silent_peers();
            next_heartbeat = now + heartbeat_interval;
        }
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.tick) {
            hooks_.tick();
        }
    }
}

void Chat::handle(const Packet& packet, std::uint32_t from) {
    enum class Event { None, Joined, Discovered, Renamed, Recolored, Left };
    Event event = Event::None;
    std::string old_name;
    std::uint64_t new_id = 0;
    {
        std::scoped_lock lock(peers_mutex_);
        if (std::ranges::find(old_ids_, packet.sender) != old_ids_.end()) {
            return;
        }
        auto it = peers_.find(packet.sender);
        if (it == peers_.end()) {
            if (packet.type == PacketType::Leave) {
                return;
            }
            it = peers_.emplace(packet.sender, Peer {packet.name, clock::now(), {}}).first;
            event = packet.type == PacketType::Join ? Event::Joined : Event::Discovered;
        } else {
            // Every packet is sent once per broadcast address, so the same one can arrive several times.
            auto& seqs = it->second.recent_seqs;
            if (std::ranges::find(seqs, packet.seq) != seqs.end()) {
                return;
            }
            it->second.last_seen = clock::now();
            if (it->second.name != packet.name) {
                old_name = std::exchange(it->second.name, packet.name);
                event = Event::Renamed;
            }
        }
        auto& seqs = it->second.recent_seqs;
        seqs.push_back(packet.seq);
        if (seqs.size() > dedup_window) {
            seqs.pop_front();
        }
        if (packet.type == PacketType::Leave) {
            // A Leave naming another id is a color change, see set_color().
            const auto next = parse_id(packet.text);
            if (next && *next != packet.sender) {
                Peer peer = std::move(it->second);
                peer.recent_seqs.clear();
                peer.last_seen = clock::now();
                // Its first packet from the new id may have arrived already.
                peers_.try_emplace(*next, std::move(peer));
                new_id = *next;
                event = Event::Recolored;
            } else {
                event = Event::Left;
            }
            peers_.erase(it);
        }
    }

    const std::string who = colored_name(packet.sender, packet.name);
    switch (event) {
    case Event::Joined:
        notice(std::format("{} joined the chat", who));
        break;
    case Event::Discovered:
        notice(std::format("{} is here", who));
        break;
    case Event::Renamed:
        notice(std::format("{} is now known as {}", colored_name(packet.sender, old_name), who));
        break;
    case Event::Recolored:
        notice(std::format("{} changed color to {}", who, colored_name(new_id, packet.name)));
        break;
    case Event::Left:
        notice(std::format("{} left the chat", who));
        break;
    case Event::None:
        break;
    }

    if (packet.type == PacketType::Join || packet.type == PacketType::Here) {
        receive_presence(packet.sender, packet.text, from);
    }
    switch (packet.type) {
    case PacketType::Join:
        // Let the newcomer know we are here, and about our channels.
        send(PacketType::Here);
        send_channel_states();
        break;
    case PacketType::Message: {
        // A move of a game (a race's words) goes to the games log, where there is one.
        bool move = false;
        if (games_apart_) {
            std::scoped_lock lock(hooks_mutex_);
            move = hooks_.claims && hooks_.claims(packet.sender, packet.text);
        }
        deliver(std::string(move ? channel::games_log : channel::general),
                {Entry::Kind::Message, std::time(nullptr), packet.sender, packet.name,
                 std::make_shared<const std::string>(packet.text)});
        if (!move) {
            history::add({std::time(nullptr), packet.sender, packet.name, packet.text});
        }
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.message) {
            hooks_.message(packet.sender, packet.name, packet.text);
        }
        break;
    }
    case PacketType::Game: {
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.packet) {
            hooks_.packet(packet.sender, packet.name, packet.text);
        }
        break;
    }
    case PacketType::Art:
        deliver(std::string(channel::general), {Entry::Kind::Art, std::time(nullptr), packet.sender, packet.name,
                                                std::make_shared<const std::string>(packet.text)});
        break;
    case PacketType::Image:
        receive_picture(packet.sender, packet.name, packet.text);
        break;
    case PacketType::ChannelState:
        receive_channel_state(packet);
        break;
    case PacketType::ChannelMessage: {
        // "CHANNEL KIND", then the message or the drawing: for its members only.
        const auto nl = packet.text.find('\n');
        const std::string_view head = std::string_view(packet.text).substr(0, nl);
        const auto space = head.find(' ');
        if (nl == std::string::npos || space == std::string_view::npos) {
            break;
        }
        const std::string channel = channel::clean_name(head.substr(0, space));
        const std::string_view kind = head.substr(space + 1);
        if (channel.empty() || channel == channel::general || (kind != "m" && kind != "a") || !in_channel(channel)) {
            break;
        }
        deliver(channel, {kind == "m" ? Entry::Kind::Message : Entry::Kind::Art, std::time(nullptr), packet.sender,
                          packet.name, std::make_shared<const std::string>(packet.text.substr(nl + 1))});
        break;
    }
    case PacketType::Offer:
        receive_offer(packet, from);
        break;
    case PacketType::Chunk:
        receive_chunk(packet);
        break;
    case PacketType::Resend:
        resend_chunks(packet.text);
        break;
    case PacketType::Here:
    case PacketType::Leave:
        break;
    }
}

void Chat::prune_silent_peers() {
    std::vector<std::string> gone;
    {
        std::scoped_lock lock(peers_mutex_);
        const auto now = clock::now();
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (now - it->second.last_seen > peer_timeout) {
                gone.push_back(colored_name(it->first, it->second.name));
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& who : gone) {
        notice(std::format("{} vanished (timed out)", who));
    }
}

} // namespace zchat
