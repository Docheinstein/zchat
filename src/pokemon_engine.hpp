#pragma once

#include "process.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace zchat::pokemon {

// The Pokémon Showdown battle simulator, for /game pokemon: it runs with Node.js, which zchat does not need for
// anything else, so it is looked for, never required. The simulator itself is installed by zchat with npm, the first
// time it is needed, in its config folder (see config::dir()), with the bridge (src/pokemon/bridge.js) next to it.
class Engine {
public:
    // Starts finding out whether Node.js is here, in the background.
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Whether node is in the PATH: waits for the answer if it is not known yet (it is quick).
    bool has_node();

    enum class Status { Missing, Installing, Ready, Failed };
    // Whether the simulator is installed. When it is missing (or failed before), install() starts installing it.
    Status status();
    // Why the last install failed.
    std::string failure();
    // Installs the simulator in the background, or its latest version again when update is true; status() tells
    // when it is done. Does nothing while installing.
    void install(bool update = false);

    // Runs a battle of Showdown's Random Battles of a generation (1 to 9) between two players (their names as the
    // battle shows them), see bridge.js for what it says and takes. nullptr, with why in error, when it cannot be
    // started.
    std::unique_ptr<process::Child> start_battle(int gen, const std::string& p1, const std::string& p2,
                                                 std::function<void(std::string line)> on_line,
                                                 std::function<void(int exit_code, std::string errors)> on_exit,
                                                 std::string& error);

private:
    std::filesystem::path dir() const;
    bool installed() const;

    std::mutex mutex_;
    std::optional<bool> node_;
    std::jthread node_check_;
    Status status_ = Status::Missing;
    std::string failure_;
    std::jthread installer_;
};

} // namespace zchat::pokemon
