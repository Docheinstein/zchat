#include "store.hpp"

#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <system_error>

namespace zchat::store {

namespace {

    // A day of leeway around max_age, see Store::keys().
    constexpr std::time_t leeway = 24 * 60 * 60;

    std::uint64_t fnv1a(std::string_view data, std::uint64_t hash = 0xcbf29ce484222325ULL) {
        for (const char c : data) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    std::optional<std::uint64_t> parse_hex(std::string_view s) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    bool told(const Record& r, std::time_t now) {
        return !r.local && r.time > now - max_age;
    }

} // namespace

std::string key_of(std::uint64_t sender, std::uint64_t seq, std::string_view data) {
    return std::format("{:x}-{}-{:08x}", sender, seq, static_cast<std::uint32_t>(fnv1a(data)));
}

bool valid_key(std::string_view key) {
    return !key.empty() && key.size() <= max_key && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == ':';
    });
}

bool valid_topic(std::string_view topic) {
    if (topic.starts_with('~')) {
        topic.remove_prefix(1);
    }
    return !topic.empty() && topic.size() <= 40 && std::ranges::all_of(topic, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string encode(const Record& r) {
    return std::format("[{},{},{},\"{:x}\",\"{:x}\",{},{},{},{}]", json::quote(r.topic), json::quote(r.key), r.time,
                       r.sender, r.user, json::quote(r.name), json::quote(std::string_view(&r.kind, 1)),
                       json::quote(r.data), r.local ? 1 : 0);
}

std::optional<Record> decode(std::string_view line) {
    const auto v = json::parse(line);
    if (!v || !v->is_array() || v->size() != 9) {
        return std::nullopt;
    }
    const json::Value& a = *v;
    for (const std::size_t i : {0, 1, 3, 4, 5, 6, 7}) {
        if (!a[i].is_string()) {
            return std::nullopt;
        }
    }
    if (!a[2].is_number() || !a[8].is_number()) {
        return std::nullopt;
    }
    Record r;
    r.topic = std::string(a[0].str());
    r.key = std::string(a[1].str());
    r.time = static_cast<std::time_t>(a[2].number);
    const auto sender = parse_hex(a[3].str());
    const auto user = parse_hex(a[4].str());
    r.name = std::string(a[5].str());
    const std::string_view kind = a[6].str();
    r.data = std::string(a[7].str());
    r.local = a[8].number != 0;
    if (!sender || !user || kind.size() != 1 || std::string_view("mag").find(kind[0]) == std::string_view::npos ||
        !valid_key(r.key) || !valid_topic(r.topic) || r.time <= 0) {
        return std::nullopt;
    }
    r.sender = *sender;
    r.user = *user;
    r.kind = kind[0];
    return r;
}

Store::Store(std::filesystem::path file, std::time_t now) :
    file_(std::move(file)) {
    if (file_.empty()) {
        return;
    }
    std::ifstream in(file_, std::ios::binary);
    std::size_t lines = 0;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        ++lines;
        if (auto r = decode(line); r && !keys_.contains(r->key)) {
            keys_.insert(r->key);
            auto key = r->key;
            topics_[r->topic][std::move(key)] = std::move(*r);
        }
    }
    forget_old_locked(now);
    // Written again, without what was forgotten (or repeated, or broken), when there was any.
    if (lines != keys_.size()) {
        save_all_locked();
    }
}

void Store::forget_old_locked(std::time_t now) {
    for (auto& [topic, records] : topics_) {
        std::erase_if(records, [&](const auto& kv) {
            const bool old = kv.second.time <= now - max_age - leeway;
            if (old) {
                keys_.erase(kv.first);
            }
            return old;
        });
        // The oldest first, past the most kept.
        while (records.size() > max_per_topic) {
            const auto oldest = std::ranges::min_element(records, {}, [](const auto& kv) {
                return kv.second.time;
            });
            keys_.erase(oldest->first);
            records.erase(oldest);
        }
    }
    std::erase_if(topics_, [](const auto& kv) {
        return kv.second.empty();
    });
}

void Store::save_all_locked() const {
    if (file_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& [topic, records] : topics_) {
            for (const auto& [key, r] : records) {
                out << encode(r) << '\n';
            }
        }
        if (!out.flush()) {
            return;
        }
    }
    std::filesystem::rename(tmp, file_, ec);
}

bool Store::add(Record record, std::time_t now) {
    if (record.time <= now - max_age || !valid_key(record.key) || !valid_topic(record.topic)) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (!keys_.insert(record.key).second) {
        return false;
    }
    ++version_;
    if (!file_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        std::ofstream(file_, std::ios::binary | std::ios::app) << encode(record) << '\n';
    }
    auto key = record.key;
    auto& records = topics_[record.topic];
    records[std::move(key)] = std::move(record);
    // Now and then, what got too old or too many goes, and the file is written again without it.
    if (records.size() > max_per_topic || ++appended_ >= 2000) {
        appended_ = 0;
        forget_old_locked(now);
        save_all_locked();
    }
    return true;
}

bool Store::has(std::string_view key) const {
    std::scoped_lock lock(mutex_);
    return keys_.contains(key);
}

std::uint64_t Store::version() const {
    std::scoped_lock lock(mutex_);
    return version_;
}

std::vector<Record> Store::records(std::string_view topic) const {
    std::vector<Record> out;
    {
        std::scoped_lock lock(mutex_);
        const auto it = topics_.find(topic);
        if (it == topics_.end()) {
            return out;
        }
        for (const auto& [key, r] : it->second) {
            out.push_back(r);
        }
    }
    std::ranges::stable_sort(out, {}, &Record::time);
    return out;
}

std::vector<std::string> Store::topics() const {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [topic, records] : topics_) {
        out.push_back(topic);
    }
    return out;
}

std::uint32_t Store::digest(std::string_view topic, std::time_t now) const {
    std::scoped_lock lock(mutex_);
    const auto it = topics_.find(topic);
    if (it == topics_.end()) {
        return 0;
    }
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    bool any = false;
    // (By key: the same order everywhere.)
    for (const auto& [key, r] : it->second) {
        if (told(r, now)) {
            hash = fnv1a(key, hash);
            hash = fnv1a("\n", hash);
            any = true;
        }
    }
    const auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return any ? (folded == 0 ? 1 : folded) : 0;
}

std::vector<std::string> Store::keys(std::string_view topic, std::time_t now) const {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    if (const auto it = topics_.find(topic); it != topics_.end()) {
        for (const auto& [key, r] : it->second) {
            if (!r.local && r.time > now - max_age - leeway) {
                out.push_back(key);
            }
        }
    }
    return out;
}

std::vector<Record> Store::missing(std::string_view topic, const std::set<std::string, std::less<>>& theirs,
                                   std::size_t max_bytes, std::time_t now) const {
    std::vector<Record> out;
    for (auto& r : records(topic)) {
        if (told(r, now) && !theirs.contains(r.key)) {
            out.push_back(std::move(r));
        }
    }
    // The newest matter most: when there are too many, the oldest are left for another time.
    std::size_t bytes = 0;
    std::size_t first = out.size();
    while (first > 0 && bytes + encode(out[first - 1]).size() + 1 <= max_bytes) {
        bytes += encode(out[--first]).size() + 1;
    }
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(first));
    return out;
}

Queue::Queue(std::filesystem::path file) :
    file_(std::move(file)) {
    if (file_.empty()) {
        return;
    }
    // "DUE KEY" lines.
    std::ifstream in(file_);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto space = line.find(' ');
        long long due = 0;
        if (space == std::string::npos ||
            std::from_chars(line.data(), line.data() + space, due).ptr != line.data() + space) {
            continue;
        }
        if (const std::string key = line.substr(space + 1); !key.empty()) {
            due_[key] = static_cast<std::time_t>(due);
        }
    }
}

void Queue::save_locked() const {
    if (file_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream out(file_, std::ios::trunc);
    for (const auto& [key, due] : due_) {
        out << static_cast<long long>(due) << ' ' << key << '\n';
    }
}

void Queue::add(std::string key, std::time_t due) {
    std::scoped_lock lock(mutex_);
    if (key.empty() || key.find('\n') != std::string::npos || due_.contains(key)) {
        return;
    }
    due_[std::move(key)] = due;
    save_locked();
}

std::vector<std::string> Queue::take_due(std::string_view prefix, std::time_t now) {
    std::scoped_lock lock(mutex_);
    std::vector<std::pair<std::time_t, std::string>> due;
    for (const auto& [key, when] : due_) {
        if (when <= now && key.starts_with(prefix)) {
            due.emplace_back(when, key);
        }
    }
    if (due.empty()) {
        return {};
    }
    std::ranges::sort(due);
    std::vector<std::string> out;
    for (auto& [when, key] : due) {
        due_.erase(key);
        out.push_back(std::move(key));
    }
    save_locked();
    return out;
}

bool Queue::waiting(std::string_view key) const {
    std::scoped_lock lock(mutex_);
    return due_.contains(key);
}

} // namespace zchat::store
