#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zchat::config {

// The per-user zchat config folder: %APPDATA%\zchat on Windows, $XDG_CONFIG_HOME/zchat (or ~/.config/zchat)
// elsewhere. Empty when it cannot be determined.
std::filesystem::path dir();

// The config file, inside dir().
std::filesystem::path file();

// Reads a value from the config file; nullopt when the file or the key is missing.
std::optional<std::string> get(std::string_view key);

// Stores a value in the config file, creating the folder and the file if needed; other keys are kept.
// Returns false when the file cannot be written.
bool set(std::string_view key, std::string_view value);

// Deletes a value from the config file, keeping the other keys. Returns false when the file cannot be written.
bool remove(std::string_view key);

} // namespace zchat::config
