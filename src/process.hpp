#pragma once

#include <chrono>
#include <stop_token>
#include <string>
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

} // namespace zchat::process
