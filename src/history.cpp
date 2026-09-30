#include "history.hpp"

#include "config.hpp"
#include "protocol.hpp"
#include "text.hpp"

#include <charconv>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>

namespace zchat::history {

namespace {

    // One message per line: time (seconds since 1970), sender id (hex), name and text, separated by tabs. Names and
    // texts are sanitized, so they hold neither tabs nor line breaks.
    std::filesystem::path file() {
        const auto dir = config::dir();
        return dir.empty() ? dir : dir / "history";
    }

    std::optional<Entry> parse(std::string_view line) {
        std::string_view fields[4];
        for (std::size_t i = 0; i < 3; ++i) {
            const auto tab = line.find('\t');
            if (tab == std::string_view::npos) {
                return std::nullopt;
            }
            fields[i] = line.substr(0, tab);
            line.remove_prefix(tab + 1);
        }
        fields[3] = line;
        Entry entry;
        long long time = 0;
        const auto [t_end, t_ec] = std::from_chars(fields[0].data(), fields[0].data() + fields[0].size(), time);
        const auto [s_end, s_ec] =
            std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), entry.sender, 16);
        if (t_ec != std::errc {} || t_end != fields[0].data() + fields[0].size() || s_ec != std::errc {} ||
            s_end != fields[1].data() + fields[1].size() || fields[2].empty()) {
            return std::nullopt;
        }
        entry.time = static_cast<std::time_t>(time);
        // Sanitized again: the file could have been edited by hand.
        entry.name = text::sanitize(fields[2], max_name_bytes);
        entry.text = text::sanitize(fields[3], max_text_bytes);
        return entry;
    }

    // The last max_entries messages of the file, and how many lines it has.
    std::deque<Entry> read(std::size_t* lines = nullptr) {
        std::deque<Entry> entries;
        std::ifstream in(file(), std::ios::binary);
        std::string line;
        std::size_t count = 0;
        while (std::getline(in, line)) {
            ++count;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (auto entry = parse(line)) {
                entries.push_back(std::move(*entry));
                if (entries.size() > max_entries) {
                    entries.pop_front();
                }
            }
        }
        if (lines) {
            *lines = count;
        }
        return entries;
    }

    void write_line(std::ostream& out, const Entry& e) {
        out << static_cast<long long>(e.time) << '\t' << std::hex << e.sender << std::dec << '\t' << e.name << '\t'
            << e.text << '\n';
    }

    std::mutex mutex;

} // namespace

void add(Entry entry) {
    entry.name = text::sanitize(entry.name, max_name_bytes);
    entry.text = text::sanitize(entry.text, max_text_bytes);
    const auto path = file();
    if (path.empty() || entry.name.empty()) {
        return;
    }
    std::scoped_lock lock(mutex);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Appended, one line at a time: another zchat on this computer can add to the same file without either losing
    // what the other wrote.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        write_line(out, entry);
        if (!out.flush()) {
            return;
        }
    }
    // Now and then, cut back to the last max_entries, through a temporary file moved over the old one, so a crash
    // never leaves half of it.
    std::size_t lines = 0;
    const auto entries = read(&lines);
    if (lines <= 2 * max_entries) {
        return;
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const Entry& e : entries) {
            write_line(out, e);
        }
        if (!out.flush()) {
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
}

std::vector<Entry> load() {
    std::scoped_lock lock(mutex);
    const auto entries = read();
    return {entries.begin(), entries.end()};
}

} // namespace zchat::history
