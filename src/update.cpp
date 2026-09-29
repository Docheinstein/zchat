#include "update.hpp"

#include "commit.hpp"
#include "process.hpp"
#include "text.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace zchat::update {

namespace {

    // Long enough for a slow network, short enough not to hang forever on a remote that asks for a password.
    constexpr std::chrono::seconds git_timeout {60};
    // How much of the output of a failed step is shown.
    constexpr std::size_t shown_lines = 10;
    // How many of the new commits are listed.
    constexpr std::size_t shown_commits = 5;

    fs::path utf8_path(std::string_view s) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    }

    std::string utf8(const fs::path& path) {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path find_executable() {
#ifdef _WIN32
        std::wstring buffer(MAX_PATH, L'\0');
        while (true) {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0) {
                return {};
            }
            if (n < buffer.size()) {
                buffer.resize(n);
                return fs::path(buffer);
            }
            buffer.resize(buffer.size() * 2);
        }
#else
        std::error_code ec;
        return fs::read_symlink("/proc/self/exe", ec);
#endif
    }

    // The running executable, as it was at startup: an update renames it.
    fs::path& executable() {
        static fs::path path = find_executable();
        return path;
    }

    fs::path with_suffix(fs::path path, std::string_view suffix) {
        path += suffix;
        return path;
    }

    // The last non-blank lines of a program's output, safe to print.
    std::vector<std::string> last_lines(std::string_view output, std::size_t count) {
        std::vector<std::string> lines;
        while (!output.empty()) {
            const auto end = output.find('\n');
            std::string_view line = output.substr(0, end);
            output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
            if (line.find_first_not_of(" \t\r") != std::string_view::npos) {
                lines.push_back(text::sanitize(line, 200));
            }
        }
        if (lines.size() > count) {
            lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
        }
        return lines;
    }

    std::string trimmed(std::string_view s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

#ifdef _WIN32
    // The old zchat waits for the new one: Ctrl+C and Ctrl+Break are for the new one only.
    BOOL WINAPI ignore_interrupts(DWORD event) {
        return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
    }
#endif

} // namespace

Updater::Updater(std::function<void(std::string_view)> notice, std::function<void()> ready) :
    notice_(std::move(notice)),
    ready_(std::move(ready)) {
}

Updater::~Updater() {
    thread_.request_stop();
}

bool Updater::start() {
    if (running_.exchange(true)) {
        return false;
    }
    // The previous update, if any, is over: this only waits for its thread to exit.
    if (thread_.joinable()) {
        thread_.join();
    }
    thread_ = std::jthread([this](std::stop_token stop) {
        run(stop);
        running_ = false;
    });
    return true;
}

void Updater::run(std::stop_token stop) {
    const fs::path source = utf8_path(ZCHAT_SOURCE_DIR);
    const fs::path build = utf8_path(ZCHAT_BUILD_DIR);
    const std::string built_commit = ZCHAT_COMMIT;
    std::error_code ec;
    if (executable().empty()) {
        notice_("Cannot update: cannot find where the zchat executable is.");
        return;
    }
    if (!fs::exists(source / ".git", ec)) {
        notice_(std::format("Cannot update: zchat was built from {}, which is not a git repository anymore.",
                            utf8(source)));
        return;
    }

    // Never ask for a password: there is nobody to type it, the terminal belongs to the chat.
#ifdef _WIN32
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
#endif
    auto git = [&](std::vector<std::string> args, std::chrono::milliseconds timeout = {}) {
        args.insert(args.begin(), {ZCHAT_GIT, "-C", utf8(source)});
        return process::run(args, stop, timeout);
    };
    auto fail = [&](std::string_view what, const process::Result& result) {
        if (stop.stop_requested()) {
            return;
        }
        notice_(what);
        if (result.timed_out) {
            notice_("  (it took too long: maybe the remote is asking for a password?)");
        }
        for (const auto& line : last_lines(result.output, shown_lines)) {
            notice_(std::format("  {}", line));
        }
    };

    notice_("Checking for updates...");
    if (const auto fetch = git({"fetch", "--quiet"}, git_timeout); fetch.exit_code != 0) {
        fail("Cannot check for updates: fetching from the remote failed.", fetch);
        return;
    }
    const auto head = git({"rev-parse", "HEAD"});
    const auto upstream = git({"rev-parse", "--verify", "--quiet", "@{upstream}"});
    if (head.exit_code != 0 || upstream.exit_code != 0) {
        fail(std::format("Cannot update: the current branch in {} does not follow a remote branch.", utf8(source)),
             head.exit_code != 0 ? head : upstream);
        return;
    }
    const std::string head_commit = trimmed(head.output);
    const std::string upstream_commit = trimmed(upstream.output);

    if (head_commit != upstream_commit) {
        if (git({"merge-base", "--is-ancestor", "HEAD", "@{upstream}"}).exit_code != 0) {
            if (!stop.stop_requested()) {
                notice_(std::format("Cannot update: the branch in {} has commits that are not on the remote. "
                                    "Update it by hand, then /update again.",
                                    utf8(source)));
            }
            return;
        }
        const auto status = git({"status", "--porcelain", "--untracked-files=no"});
        if (status.exit_code != 0 || !trimmed(status.output).empty()) {
            if (!stop.stop_requested()) {
                notice_(std::format("Cannot update: {} has uncommitted changes. Commit or stash them, then /update "
                                    "again.",
                                    utf8(source)));
            }
            return;
        }
        const auto log = git({"log", "--oneline", "--no-decorate", "HEAD..@{upstream}"});
        const auto commits = last_lines(log.output, static_cast<std::size_t>(-1));
        notice_(std::format("{} new commit{}:", commits.size(), commits.size() == 1 ? "" : "s"));
        for (std::size_t i = 0; i < commits.size() && i < shown_commits; ++i) {
            notice_(std::format("  {}", commits[i]));
        }
        if (commits.size() > shown_commits) {
            notice_(std::format("  ... and {} more", commits.size() - shown_commits));
        }
        if (const auto merge = git({"merge", "--ff-only", "--quiet", "@{upstream}"}); merge.exit_code != 0) {
            fail("Cannot update: pulling the new commits failed.", merge);
            return;
        }
    } else if (built_commit.empty() || built_commit == head_commit) {
        notice_("zchat is up to date.");
        return;
    } else {
        // Pulled by hand, or a previous /update pulled but could not build: the sources are ahead of this zchat.
        notice_("The sources are newer than this zchat.");
    }

    notice_("Building the new zchat...");
    const fs::path running = executable();
    const fs::path built = utf8_path(ZCHAT_BUILT_EXE);
    const fs::path old = with_suffix(running, ".old");
    const std::string config = ZCHAT_BUILD_CONFIG;
    // When zchat runs straight from the build folder, the build must write where the running executable is:
    // Windows does not allow that, so move it out of the way first (a running executable can be renamed).
    const bool in_place = fs::equivalent(running, built, ec);
    if (in_place) {
        fs::remove(old, ec);
        fs::rename(running, old, ec);
        if (ec) {
            notice_(std::format("Cannot update: cannot rename {}: {}", utf8(running), ec.message()));
            return;
        }
    }
    auto restore = [&] {
        if (in_place && !fs::exists(running, ec)) {
            fs::rename(old, running, ec);
        }
    };

    if (!fs::exists(build / "CMakeCache.txt", ec)) {
        std::vector<std::string> configure {ZCHAT_CMAKE, "-S", utf8(source), "-B", utf8(build)};
        if (!config.empty()) {
            configure.push_back("-DCMAKE_BUILD_TYPE=" + config);
        }
        if (const auto result = process::run(configure, stop); result.exit_code != 0) {
            restore();
            fail("Cannot update: configuring the build failed. zchat was not changed.", result);
            return;
        }
    }
    std::vector<std::string> build_command {ZCHAT_CMAKE, "--build", utf8(build), "--target", "zchat", "--parallel"};
    if (!config.empty()) {
        build_command.insert(build_command.end(), {"--config", config});
    }
    if (const auto result = process::run(build_command, stop); result.exit_code != 0) {
        restore();
        fail("Cannot update: the build failed. zchat was not changed.", result);
        return;
    }

    if (in_place) {
        // Works on Linux; on Windows it is deleted by the next zchat that starts.
        fs::remove(old, ec);
    } else {
        // Copy next to the running executable first, so that it is replaced in one step.
        const fs::path fresh = with_suffix(running, ".new");
        fs::copy_file(built, fresh, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
#ifdef _WIN32
            fs::remove(old, ec);
            fs::rename(running, old, ec);
#endif
            if (!ec) {
                fs::rename(fresh, running, ec);
            }
        }
        if (ec) {
            notice_(std::format("Cannot update: cannot replace {}: {}", utf8(running), ec.message()));
            std::error_code ignored;
            if (!fs::exists(running, ignored)) {
                fs::rename(old, running, ignored);
            }
            fs::remove(fresh, ignored);
            return;
        }
    }

    notice_("zchat is updated: restarting...");
    ready_();
}

void clean_up() {
    const fs::path& running = executable();
    if (!running.empty()) {
        std::error_code ec;
        fs::remove(with_suffix(running, ".old"), ec);
    }
}

int restart(char** argv) {
    const fs::path& exe = executable();
    std::fflush(stdout);
#ifdef _WIN32
    (void)argv;
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION pi {};
    std::wstring command = GetCommandLineW();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &pi)) {
        std::fprintf(stderr, "zchat: cannot restart %s (error %lu)\n", utf8(exe).c_str(), GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    // Windows has no exec: stay around until the new zchat ends, or the shell would take the console back.
    SetConsoleCtrlHandler(ignore_interrupts, TRUE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    execv(exe.c_str(), argv);
    std::fprintf(stderr, "zchat: cannot restart %s: %s\n", exe.c_str(), std::strerror(errno));
    return 1;
#endif
}

} // namespace zchat::update
