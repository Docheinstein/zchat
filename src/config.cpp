#include "config.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace zchat::config {

namespace {

    using Values = std::map<std::string, std::string, std::less<>>;

    // The file is made of "key=value" lines; blank lines and lines starting with '#' are ignored.
    Values load() {
        Values values;
        std::ifstream in(file());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto eq = line.find('=');
            if (line.empty() || line.front() == '#' || eq == std::string::npos) {
                continue;
            }
            values[line.substr(0, eq)] = line.substr(eq + 1);
        }
        return values;
    }

    bool save(const std::filesystem::path& path, const Values& values) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        // Write a temporary file and move it over the old one, so a crash never leaves a half-written config.
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << "# zchat settings\n";
            for (const auto& [k, v] : values) {
                out << k << '=' << v << '\n';
            }
            if (!out.flush()) {
                return false;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }

} // namespace

std::filesystem::path dir() {
    // Another folder, for a second profile (e.g. two people on one computer, or tests).
#ifdef _WIN32
    if (const DWORD size = GetEnvironmentVariableW(L"ZCHAT_CONFIG_DIR", nullptr, 0); size > 1) {
        std::wstring custom(size, wchar_t {});
        custom.resize(GetEnvironmentVariableW(L"ZCHAT_CONFIG_DIR", custom.data(), size));
        return std::filesystem::path(custom);
    }
    PWSTR appdata = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        base = appdata;
    }
    CoTaskMemFree(appdata);
    return base.empty() ? base : base / "zchat";
#else
    if (const char* custom = std::getenv("ZCHAT_CONFIG_DIR"); custom && *custom) {
        return std::filesystem::path(custom);
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "zchat";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "zchat";
    }
    return {};
#endif
}

std::filesystem::path file() {
    const auto folder = dir();
    return folder.empty() ? folder : folder / "config";
}

std::optional<std::string> get(std::string_view key) {
    const auto values = load();
    if (const auto it = values.find(key); it != values.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool set(std::string_view key, std::string_view value) {
    const auto path = file();
    if (path.empty()) {
        return false;
    }
    auto values = load();
    values[std::string(key)] = std::string(value);
    return save(path, values);
}

bool remove(std::string_view key) {
    const auto path = file();
    if (path.empty()) {
        return false;
    }
    auto values = load();
    const auto it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    values.erase(it);
    return save(path, values);
}

} // namespace zchat::config
