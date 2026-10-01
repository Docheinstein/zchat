#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::process {

struct Result {
    // The exit code of the program, or -1 when it could not be started or did not end by itself.
    int exit_code = -1;
    // What it printed, stdout and stderr together (only the end, when it printed a lot).
    std::string output;
    bool timed_out = false;
};

// Runs a program (args[0], searched in the PATH, with the following args; all UTF-8) and waits for it to end.
// It gets no input and no console of its own, so it can never mess with the chat on screen or wait for the user.
// When stop is requested, or after timeout (if not zero), it is killed together with everything it started.
Result run(const std::vector<std::string>& args, std::stop_token stop,
           std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

// A program kept running, talking with zchat in lines of text: written to its standard input, and read from its
// standard output (like node running the Pokémon battle bridge). Like run(), it has no console of its own, and it is
// killed together with everything it started when this goes away (or zchat does).
class Child {
public:
    // Each line the program prints (without the line break) goes to on_line; then, once it has ended and printed
    // everything, on_exit gets its exit code (-1 when killed) and the end of what it printed on its standard error.
    // Both are called on a thread of the Child's own, never at the same time. Returns nullptr, with why in error,
    // when it cannot be started.
    static std::unique_ptr<Child> start(const std::vector<std::string>& args,
                                        std::function<void(std::string line)> on_line,
                                        std::function<void(int exit_code, std::string errors)> on_exit,
                                        std::string& error);

    // Closes its standard input, gives it a moment to end by itself, then kills it; on_line and on_exit are not
    // called anymore once it returns.
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    // Writes a line (a line break is added) to its standard input. Returns false once it is gone. Thread-safe.
    bool write(std::string_view line);

    struct Impl;

private:
    explicit Child(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace zchat::process
