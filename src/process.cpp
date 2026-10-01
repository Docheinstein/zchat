#include "process.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

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

// --- Child

namespace {

    // Splits what a program prints into lines, without their "\r\n" or "\n".
    class LineSplitter {
    public:
        explicit LineSplitter(std::function<void(std::string)>& on_line) :
            on_line_(on_line) {
        }
        void add(const char* data, std::size_t size) {
            pending_.append(data, size);
            std::size_t start = 0;
            for (auto nl = pending_.find('\n'); nl != std::string::npos; nl = pending_.find('\n', start)) {
                std::string line = pending_.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                start = nl + 1;
                on_line_(std::move(line));
            }
            pending_.erase(0, start);
        }
        void finish() {
            if (!pending_.empty()) {
                on_line_(std::exchange(pending_, {}));
            }
        }

    private:
        std::function<void(std::string)>& on_line_;
        std::string pending_;
    };

    // How long a program gets to end by itself once its input is closed, before it is killed.
    constexpr auto child_grace = std::chrono::milliseconds(1500);

} // namespace

struct Child::Impl {
    std::function<void(std::string)> on_line;
    std::function<void(int, std::string)> on_exit;
    // Once set, the callbacks are not called anymore (the Child is going away).
    std::atomic<bool> cancelled {false};
    std::mutex write_mutex;
    std::string errors;
    std::mutex errors_mutex;
    std::jthread reader;
    std::jthread error_reader;
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE error_output = nullptr;
#else
    pid_t pid = 0;
    int input = -1;
    int output = -1;
    int error_output = -1;
    std::atomic<bool> reaped {false};
    int status = 0;
#endif

    // On the reader thread: the lines, then the end.
    void read_all();
    int wait_for_exit();
};

#ifdef _WIN32
void Child::Impl::read_all() {
    {
        std::function<void(std::string)> deliver = [this](std::string line) {
            if (!cancelled) {
                on_line(std::move(line));
            }
        };
        LineSplitter lines(deliver);
        char buffer[8192];
        DWORD n = 0;
        while (ReadFile(output, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
            lines.add(buffer, n);
        }
        lines.finish();
    }
    if (error_reader.joinable()) {
        error_reader.join();
    }
    const int code = wait_for_exit();
    if (!cancelled && on_exit) {
        std::scoped_lock lock(errors_mutex);
        on_exit(code, errors);
    }
}

int Child::Impl::wait_for_exit() {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    return cancelled ? -1 : static_cast<int>(code);
}

std::unique_ptr<Child> Child::start(const std::vector<std::string>& args, std::function<void(std::string)> on_line,
                                    std::function<void(int, std::string)> on_exit, std::string& error) {
    if (args.empty()) {
        error = "nothing to run";
        return nullptr;
    }
    SECURITY_ATTRIBUTES inheritable {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    // Each pipe: the end for the program (inherited) and ours (not).
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr, err_read = nullptr,
           err_write = nullptr;
    const auto close_all = [&] {
        for (HANDLE h : {in_read, in_write, out_read, out_write, err_read, err_write}) {
            if (h) {
                CloseHandle(h);
            }
        }
    };
    if (!CreatePipe(&in_read, &in_write, &inheritable, 0) || !CreatePipe(&out_read, &out_write, &inheritable, 0) ||
        !CreatePipe(&err_read, &err_write, &inheritable, 0)) {
        close_all();
        error = "cannot create a pipe";
        return nullptr;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE handles[] = {in_read, out_write, err_write};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    InitializeProcThreadAttributeList(list, 1, 0, &size);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);

    STARTUPINFOEXW info {};
    info.StartupInfo.cb = sizeof(info);
    info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    info.StartupInfo.hStdInput = in_read;
    info.StartupInfo.hStdOutput = out_write;
    info.StartupInfo.hStdError = err_write;
    info.lpAttributeList = list;

    std::wstring command;
    for (const auto& arg : args) {
        append_quoted(command, widen(arg));
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    PROCESS_INFORMATION pi {};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &info.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    // The program's ends are its own now.
    CloseHandle(std::exchange(in_read, nullptr));
    CloseHandle(std::exchange(out_write, nullptr));
    CloseHandle(std::exchange(err_write, nullptr));
    if (!started) {
        close_all();
        CloseHandle(job);
        error = std::format("cannot run {}", args.front());
        return nullptr;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto impl = std::make_unique<Impl>();
    impl->on_line = std::move(on_line);
    impl->on_exit = std::move(on_exit);
    impl->process = pi.hProcess;
    impl->job = job;
    impl->input = in_write;
    impl->output = out_read;
    impl->error_output = err_read;
    Impl* p = impl.get();
    p->error_reader = std::jthread([p] {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(p->error_output, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
            std::scoped_lock lock(p->errors_mutex);
            append_output(p->errors, buffer, n);
        }
    });
    p->reader = std::jthread([p] {
        p->read_all();
    });
    return std::unique_ptr<Child>(new Child(std::move(impl)));
}

Child::~Child() {
    impl_->cancelled = true;
    {
        std::scoped_lock lock(impl_->write_mutex);
        if (impl_->input) {
            CloseHandle(std::exchange(impl_->input, nullptr));
        }
    }
    if (WaitForSingleObject(impl_->process, static_cast<DWORD>(child_grace.count())) != WAIT_OBJECT_0) {
        TerminateJobObject(impl_->job, 1);
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    if (impl_->error_reader.joinable()) {
        impl_->error_reader.join();
    }
    CloseHandle(impl_->output);
    CloseHandle(impl_->error_output);
    CloseHandle(impl_->process);
    CloseHandle(impl_->job);
}

bool Child::write(std::string_view line) {
    std::string data(line);
    data += '\n';
    std::scoped_lock lock(impl_->write_mutex);
    if (!impl_->input) {
        return false;
    }
    DWORD written = 0;
    for (std::size_t done = 0; done < data.size(); done += written) {
        if (!WriteFile(impl_->input, data.data() + done, static_cast<DWORD>(data.size() - done), &written, nullptr) ||
            written == 0) {
            CloseHandle(std::exchange(impl_->input, nullptr));
            return false;
        }
    }
    return true;
}
#else
void Child::Impl::read_all() {
    {
        std::function<void(std::string)> deliver = [this](std::string line) {
            if (!cancelled) {
                on_line(std::move(line));
            }
        };
        LineSplitter lines(deliver);
        char buffer[8192];
        while (true) {
            const auto n = ::read(output, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            lines.add(buffer, static_cast<std::size_t>(n));
        }
        lines.finish();
    }
    if (error_reader.joinable()) {
        error_reader.join();
    }
    const int code = wait_for_exit();
    if (!cancelled && on_exit) {
        std::scoped_lock lock(errors_mutex);
        on_exit(code, errors);
    }
}

int Child::Impl::wait_for_exit() {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
    return WIFEXITED(status) && !cancelled ? WEXITSTATUS(status) : -1;
}

std::unique_ptr<Child> Child::start(const std::vector<std::string>& args, std::function<void(std::string)> on_line,
                                    std::function<void(int, std::string)> on_exit, std::string& error) {
    if (args.empty()) {
        error = "nothing to run";
        return nullptr;
    }
    // Writing to a program that is gone must be an error, not a SIGPIPE that ends zchat.
    std::signal(SIGPIPE, SIG_IGN);
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    const auto close_all = [&] {
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
    };
    if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
        close_all();
        error = "cannot create a pipe";
        return nullptr;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    // A session of its own, as in run(): a process group that can be killed as a whole.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    std::vector<char*> argv;
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawn_error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(std::exchange(in[0], -1));
    close(std::exchange(out[1], -1));
    close(std::exchange(err[1], -1));
    if (spawn_error != 0) {
        close_all();
        error = std::format("cannot run {}: {}", args.front(), std::strerror(spawn_error));
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->on_line = std::move(on_line);
    impl->on_exit = std::move(on_exit);
    impl->pid = pid;
    impl->input = in[1];
    impl->output = out[0];
    impl->error_output = err[0];
    Impl* p = impl.get();
    p->error_reader = std::jthread([p] {
        char buffer[4096];
        while (true) {
            const auto n = ::read(p->error_output, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            std::scoped_lock lock(p->errors_mutex);
            append_output(p->errors, buffer, static_cast<std::size_t>(n));
        }
    });
    p->reader = std::jthread([p] {
        p->read_all();
    });
    return std::unique_ptr<Child>(new Child(std::move(impl)));
}

Child::~Child() {
    impl_->cancelled = true;
    {
        std::scoped_lock lock(impl_->write_mutex);
        if (impl_->input >= 0) {
            close(std::exchange(impl_->input, -1));
        }
    }
    // The reader reaps it: until then, its pid cannot belong to anybody else.
    const auto deadline = std::chrono::steady_clock::now() + child_grace;
    while (!impl_->reaped && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!impl_->reaped) {
        ::kill(-impl_->pid, SIGKILL);
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    if (impl_->error_reader.joinable()) {
        impl_->error_reader.join();
    }
    close(impl_->output);
    close(impl_->error_output);
}

bool Child::write(std::string_view line) {
    std::string data(line);
    data += '\n';
    std::scoped_lock lock(impl_->write_mutex);
    if (impl_->input < 0) {
        return false;
    }
    for (std::size_t done = 0; done < data.size();) {
        const auto n = ::write(impl_->input, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(std::exchange(impl_->input, -1));
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}
#endif

Child::Child(std::unique_ptr<Impl> impl) :
    impl_(std::move(impl)) {
}

// --- run

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
