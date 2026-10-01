#include "pokemon_engine.hpp"

#include "config.hpp"

#include "pokemon_bridge.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

namespace zchat::pokemon {

namespace {

    using namespace std::chrono_literals;

    constexpr auto node_check_timeout = 10s;
    // npm downloads about 30 MB: a slow network gets some time.
    constexpr auto install_timeout = 10min;

    // npm is a script on Windows (npm.cmd), which only cmd runs.
    std::vector<std::string> npm(std::vector<std::string> args) {
#ifdef _WIN32
        args.insert(args.begin(), {"cmd", "/d", "/c", "npm"});
#else
        args.insert(args.begin(), "npm");
#endif
        return args;
    }

    std::string utf8(const std::filesystem::path& path) {
        const std::u8string s = path.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }

    // The end of what a failed command printed, enough to tell why.
    std::string tail(const std::string& output) {
        std::string_view rest = output;
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r' || rest.back() == ' ')) {
            rest.remove_suffix(1);
        }
        constexpr std::size_t max = 400;
        if (rest.size() > max) {
            rest = rest.substr(rest.size() - max);
            if (const auto nl = rest.find('\n'); nl != std::string_view::npos) {
                rest.remove_prefix(nl + 1);
            }
        }
        return std::string(rest);
    }

} // namespace

Engine::Engine() {
    node_check_ = std::jthread([this](std::stop_token stop) {
        const auto result = process::run({"node", "--version"}, stop, node_check_timeout);
        std::scoped_lock lock(mutex_);
        node_ = result.exit_code == 0 && result.output.starts_with("v");
    });
    if (installed()) {
        status_ = Status::Ready;
    }
}

Engine::~Engine() = default;

bool Engine::has_node() {
    {
        std::scoped_lock lock(mutex_);
        if (node_) {
            return *node_;
        }
    }
    if (node_check_.joinable()) {
        node_check_.join();
    }
    std::scoped_lock lock(mutex_);
    return node_.value_or(false);
}

std::filesystem::path Engine::dir() const {
    const auto base = config::dir();
    return base.empty() ? std::filesystem::path() : base / "pokemon";
}

bool Engine::installed() const {
    const auto d = dir();
    std::error_code ec;
    return !d.empty() && std::filesystem::exists(d / "node_modules" / "pokemon-showdown" / "package.json", ec);
}

Engine::Status Engine::status() {
    std::scoped_lock lock(mutex_);
    return status_;
}

std::string Engine::failure() {
    std::scoped_lock lock(mutex_);
    return failure_;
}

void Engine::install(bool update) {
    std::scoped_lock lock(mutex_);
    if (status_ == Status::Installing) {
        return;
    }
    const auto d = dir();
    if (d.empty()) {
        status_ = Status::Failed;
        failure_ = "there is no config folder to install it in";
        return;
    }
    status_ = Status::Installing;
    if (installer_.joinable()) {
        installer_.request_stop();
        installer_.join();
    }
    installer_ = std::jthread([this, d, update](std::stop_token stop) {
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        // Only what the simulator needs: not the databases and the rest that the Showdown server can use.
        const auto result =
            process::run(npm({"install", update ? "pokemon-showdown@latest" : "pokemon-showdown", "--prefix", utf8(d),
                              "--omit=optional", "--ignore-scripts", "--no-audit", "--no-fund", "--loglevel=error"}),
                         stop, install_timeout);
        std::scoped_lock lock(mutex_);
        if (result.exit_code == 0 && installed()) {
            status_ = Status::Ready;
            failure_.clear();
        } else {
            status_ = Status::Failed;
            failure_ = result.timed_out        ? std::string("npm took too long")
                       : result.output.empty() ? std::string("npm could not be run")
                                               : tail(result.output);
        }
    });
}

std::unique_ptr<process::Child> Engine::start_battle(int gen, const std::string& p1, const std::string& p2,
                                                     std::function<void(std::string)> on_line,
                                                     std::function<void(int, std::string)> on_exit,
                                                     std::string& error) {
    if (!installed()) {
        error = "Pokémon Showdown is not installed";
        return nullptr;
    }
    // Written every time, so it is always this version's.
    const auto bridge = dir() / "bridge.js";
    {
        std::ofstream file(bridge, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(pokemon_bridge), static_cast<std::streamsize>(pokemon_bridge_size));
        if (!file) {
            error = std::format("cannot write {}", utf8(bridge));
            return nullptr;
        }
    }
    return process::Child::start({"node", utf8(bridge), std::format("gen{}randombattle", gen), p1, p2}, std::move(on_line),
                                 std::move(on_exit), error);
}

} // namespace zchat::pokemon
