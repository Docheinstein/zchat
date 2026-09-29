#include "process.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace zchat::process {

namespace {

    // Enough to show why a build failed, without keeping a whole build log in memory.
    constexpr std::size_t max_output = 64 * 1024;

    void append_output(std::string& output, const char* data, std::size_t size) {
        output.append(data, size);
        if (output.size() > max_output) {
            output.erase(0, output.size() - max_output);
        }
    }

#ifdef _WIN32
    std::wstring widen(const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), wide.data(), n);
        return wide;
    }

    // Quotes an argument so that the program gets it back unchanged (the rules of CommandLineToArgvW).
    void append_quoted(std::wstring& command, const std::wstring& arg) {
        if (!command.empty()) {
            command += L' ';
        }
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            command += arg;
            return;
        }
        command += L'"';
        std::size_t backslashes = 0;
        for (const wchar_t c : arg) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            // Backslashes are literal, unless they come before a quote.
            command.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            command += c;
        }
        command.append(backslashes * 2, L'\\');
        command += L'"';
    }

    Result run_program(const std::vector<std::string>& args, std::stop_token stop) {
        Result result;
        SECURITY_ATTRIBUTES inheritable {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
            result.output = "cannot create a pipe";
            return result;
        }
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                     OPEN_EXISTING, 0, nullptr);

        // Only these handles are passed down, not the chat socket or anything else of zchat.
        HANDLE handles[] = {null_in, write_end};
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> attributes(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        InitializeProcThreadAttributeList(list, 1, 0, &size);
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr,
                                  nullptr);

        STARTUPINFOEXW info {};
        info.StartupInfo.cb = sizeof(info);
        info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        info.StartupInfo.hStdInput = null_in;
        info.StartupInfo.hStdOutput = write_end;
        info.StartupInfo.hStdError = write_end;
        info.lpAttributeList = list;

        std::wstring command;
        for (const auto& arg : args) {
            append_quoted(command, widen(arg));
        }

        // A job, to kill the program together with its own children (the compiler, ssh, ...), also when zchat
        // itself goes away.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        // CREATE_NO_WINDOW: a hidden console of its own, not the one the chat is drawn on.
        PROCESS_INFORMATION pi {};
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                            nullptr, &info.StartupInfo, &pi);
        DeleteProcThreadAttributeList(list);
        CloseHandle(write_end);
        CloseHandle(null_in);
        if (!started) {
            CloseHandle(read_end);
            CloseHandle(job);
            result.output = std::format("cannot run {}", args.front());
            return result;
        }
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        {
            std::stop_callback kill(stop, [job] {
                TerminateJobObject(job, 1);
            });
            char buffer[4096];
            DWORD n = 0;
            while (ReadFile(read_end, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
                append_output(result.output, buffer, n);
            }
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = stop.stop_requested() ? -1 : static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(read_end);
        CloseHandle(job);
        return result;
    }
#else
    Result run_program(const std::vector<std::string>& args, std::stop_token stop) {
        Result result;
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            result.output = "cannot create a pipe";
            return result;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        // A session of its own: no controlling terminal, so nothing it starts (like ssh asking for a passphrase)
        // can read from or draw on the chat's terminal; and a process group that can be killed as a whole.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);

        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        pid_t pid = 0;
        const int error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if (error != 0) {
            close(fds[0]);
            result.output = std::format("cannot run {}: {}", args.front(), std::strerror(error));
            return result;
        }

        {
            // The child is not reaped before this callback is gone, so the pid cannot belong to anybody else.
            std::stop_callback kill(stop, [pid] {
                ::kill(-pid, SIGTERM);
            });
            char buffer[4096];
            while (true) {
                const auto n = ::read(fds[0], buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    break;
                }
                append_output(result.output, buffer, static_cast<std::size_t>(n));
            }
        }
        close(fds[0]);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        result.exit_code = WIFEXITED(status) && !stop.stop_requested() ? WEXITSTATUS(status) : -1;
        return result;
    }
#endif

} // namespace

Result run(const std::vector<std::string>& args, std::stop_token stop, std::chrono::milliseconds timeout) {
    if (timeout == std::chrono::milliseconds::zero()) {
        return run_program(args, stop);
    }
    // Stops the program when the caller asks, or when the timer below runs out.
    std::stop_source source;
    std::stop_callback forward(stop, [&source] {
        source.request_stop();
    });
    std::atomic<bool> timed_out {false};
    std::jthread timer([&source, &timed_out, timeout](std::stop_token done) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, done, timeout, [] {
            return false;
        });
        if (!done.stop_requested()) {
            timed_out = true;
            source.request_stop();
        }
    });
    Result result = run_program(args, source.get_token());
    timer.request_stop();
    timer.join();
    result.timed_out = timed_out;
    return result;
}

} // namespace zchat::process
