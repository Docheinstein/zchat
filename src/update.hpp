#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace zchat::update {

// Updates zchat from its git remote while it runs: zchat remembers the source folder and the build folder it was
// built from, so it can fetch, fast-forward the sources, rebuild them and replace its own executable.
class Updater {
public:
    // notice prints a line in the chat. ready is called, from the update thread, once the new zchat is in place:
    // the running one should then quit and call restart().
    Updater(std::function<void(std::string_view)> notice, std::function<void()> ready);
    // Cancels an update in progress, stopping git or the build.
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Starts an update in the background. Returns false, doing nothing, when one is already in progress. During a
    // check(), the update starts as soon as the check is over.
    bool start();

    // Checks in the background whether there is a newer zchat, and installs it when there is, so that everybody
    // in the chat runs the same, newest one. When it cannot install it by itself (the clone has local commits or
    // uncommitted changes), it says so instead, and that /update installs it. Quiet when it cannot tell, and it
    // tries each new version only once: a build that fails is not tried again until there is a newer one.
    void check();

    // check() now, and again every few minutes while zchat runs.
    void watch();

private:
    void run(std::stop_token stop);
    // Whether the update should be installed now.
    bool run_check(std::stop_token stop);

    std::function<void(std::string_view)> notice_;
    std::function<void()> ready_;
    std::atomic<bool> running_ {false};
    // A check() is in progress, and an update was asked for during it.
    std::mutex mutex_;
    bool checking_ = false;
    std::atomic<bool> update_after_check_ {false};
    // The remote commit check() last told about or tried to install, so that it does so once.
    std::string checked_;
    std::jthread thread_;
    // Runs check() every few minutes, see watch().
    std::jthread timer_;
};

// To be called first thing at startup: remembers where the executable is, and deletes the old executable left
// behind by a previous update (Windows cannot delete an executable while it runs).
void clean_up();

// Starts the updated zchat with the same arguments, in the same terminal. On Linux it replaces this process and
// returns only on failure (with 1); on Windows it runs as a child, and its exit code is returned when it ends.
int restart(char** argv);

} // namespace zchat::update
