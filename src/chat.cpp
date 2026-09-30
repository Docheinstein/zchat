#include "chat.hpp"

#include "cipher.hpp"
#include "file.hpp"
#include "image.hpp"
#include "markup.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
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
    socket_(port) {
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
    send(PacketType::Message, clean);
    print_message(id_, name(), clean);
    history::add({std::time(nullptr), id_, name(), clean});
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
    send(PacketType::Art, clean);
    print_art(id_, name(), clean);
}

void Chat::send_picture(std::string_view picture) {
    Packet packet;
    packet.type = PacketType::Image;
    packet.name = name();
    packet.text = std::string(picture);
    const std::string clean = decode(encode(packet)).value_or(Packet {}).text;
    print_picture(id_, name(), clean);
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
    // Files travel like pictures: print_picture() tells them apart.
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
    print_picture(key.first, name, text);
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
    std::scoped_lock lock(avatars_mutex_);
    if (!own_avatar_ || !listener_) {
        return {};
    }
    return std::format("avatar {:016x} {} {}", own_avatar_hash_, own_avatar_->size(), listener_->port());
}

void Chat::receive_presence(std::uint64_t sender, std::string_view text, std::uint32_t from) {
    // "avatar HASH BYTES PORT", or nothing for no avatar (and from older versions).
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

void Chat::print_history(const std::vector<history::Entry>& entries) const {
    for (const auto& entry : entries) {
        // Tags are shown, but ring no bell: they were heard back then.
        print_message(entry.sender, entry.name, entry.text, entry.time);
    }
}

bool Chat::print_message(std::uint64_t id, std::string_view name, std::string_view text,
                         std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    bool tags_us = false;
    const std::string marked = mark_mentions(text, tags_us);
    terminal_.print(
        std::format("{} {}: {}", time, colored_name(id, name), markup::render(marked, terminal_.colors())));
    return tags_us;
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

void Chat::print_art(std::uint64_t id, std::string_view name, std::string_view art) const {
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", timestamp()) : timestamp();
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

void Chat::print_picture(std::uint64_t id, std::string_view name, std::string_view text) {
    if (file::is_file(text)) {
        print_file(id, name, text);
        return;
    }
    const auto picture = image::parse_picture(text);
    if (!picture) {
        return;
    }
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", timestamp()) : timestamp();
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
        print_art(id, name, *art);
    }
}

void Chat::print_file(std::uint64_t id, std::string_view name, std::string_view text) {
    const auto received = file::parse(text);
    if (!received) {
        return;
    }
    const std::string who = colored_name(id, name);
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
        notice(std::format("A file from {} could not be kept: {}", who, received->name));
        return;
    }
    const std::string time = terminal_.colors() ? std::format("[90m{}[0m", timestamp()) : timestamp();
    const std::string size = file::format_size(received->data.size());
    if (terminal_.show_file(std::format("{} {}:", time, who), index, received->name, size)) {
        return;
    }
    const std::string hint = std::format("/save {} to download", index);
    terminal_.print(std::format("{} {}: file {} ({}), {}", time, who, received->name, size,
                                terminal_.colors() ? std::format("[90m{}[0m", hint) : hint));
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

void Chat::notice(std::string_view text) const {
    if (terminal_.colors()) {
        terminal_.print(std::format("\x1b[90m{} *\x1b[0m {}", timestamp(), text));
    } else {
        terminal_.print(std::format("{} * {}", timestamp(), text));
    }
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
    while (!stop.stop_requested()) {
        if (auto datagram = socket_.receive(200ms)) {
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
        // Let the newcomer know we are here.
        send(PacketType::Here);
        break;
    case PacketType::Message: {
        if (print_message(packet.sender, packet.name, packet.text)) {
            terminal_.bell();
        }
        history::add({std::time(nullptr), packet.sender, packet.name, packet.text});
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
        print_art(packet.sender, packet.name, packet.text);
        break;
    case PacketType::Image:
        print_picture(packet.sender, packet.name, packet.text);
        break;
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
