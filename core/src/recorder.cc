#include "teleprompter/recorder.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tp {
namespace {

namespace fs = std::filesystem;

double monotonic_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string timestamp_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H%M%S", &local);
    return buffer;
}


int status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int wait_with_timeout(pid_t pid, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) return status_code(status);
        if (result < 0 && errno != EINTR) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -2; // still alive
}

bool read_all(int fd, std::string& out) {
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

std::vector<char*> make_argv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    return argv;
}

int run_capture(const std::vector<std::string>& args, std::string& stderr_text,
                int timeout_seconds) {
    int errpipe[2] = {-1, -1};
    if (::pipe(errpipe) != 0) return -1;
    std::vector<char*> argv = make_argv(args);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(errpipe[0]);
        ::close(errpipe[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(errpipe[0]);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::close(devnull);
        }
        ::dup2(errpipe[1], STDERR_FILENO);
        ::close(errpipe[1]);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(errpipe[1]);
    std::thread reader([fd = errpipe[0], &stderr_text] {
        read_all(fd, stderr_text);
        ::close(fd);
    });
    int code = wait_with_timeout(pid, timeout_seconds);
    if (code == -2) {
        ::kill(pid, SIGTERM);
        code = wait_with_timeout(pid, 3);
        if (code == -2) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            code = status_code(status);
        }
    }
    reader.join();
    return code;
}

} // namespace

std::vector<std::string> build_ffmpeg_cmd(const std::string& out_path,
                                          const RecorderOptions& options) {
    return {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostats",
        "-f", "v4l2", "-input_format", "mjpeg",
        "-video_size", options.size, "-framerate", std::to_string(options.fps),
        "-i", options.device,
        "-f", "pulse", "-i", options.audio,
        "-c:v", "libx264", "-preset", "veryfast", "-crf", std::to_string(options.crf),
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "160k",
        "-movflags", "+faststart",
        "-y", out_path,
    };
}

std::string pick_error_line(const std::string& stderr_text, std::size_t limit) {
    std::vector<std::string> lines;
    std::istringstream input(stderr_text);
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        const std::size_t end = line.find_last_not_of(" \t\r\n");
        lines.push_back(line.substr(begin, end - begin + 1));
    }
    if (lines.empty()) return {};

    static const std::regex hint(
        R"((cannot|busy|no such|not found|permission|denied|invalid|unable|failed|error))",
        std::regex::icase);
    std::string best = lines.front();
    for (const std::string& candidate : lines) {
        if (std::regex_search(candidate, hint)) {
            best = candidate;
            break;
        }
    }
    // Strip ffmpeg's "[in#0 @ 0x…] " component prefix.
    if (!best.empty() && best.front() == '[') {
        const std::size_t close = best.find(']');
        if (close != std::string::npos) {
            std::size_t start = close + 1;
            while (start < best.size() && std::isspace(static_cast<unsigned char>(best[start]))) ++start;
            best.erase(0, start);
        }
    }
    if (best.size() > limit) best.resize(limit);
    return best;
}

bool parse_capture_size(const std::string& value, int& width, int& height,
                        std::string& error) {
    static const std::regex pattern(R"(^(\d{2,5})x(\d{2,5})$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) {
        error = "expected WIDTHxHEIGHT, e.g. 1280x720 (got '" + value + "')";
        return false;
    }
    width = std::stoi(match[1].str());
    height = std::stoi(match[2].str());
    if ((width % 2) != 0 || (height % 2) != 0) {
        error = value + ": width and height must be even for H.264 (e.g. 1280x720)";
        return false;
    }
    return true;
}

int check_devices(const RecorderOptions& options) {
    struct Check {
        std::string label;
        std::vector<std::string> inputs;
    };
    const std::vector<Check> checks = {
        {"camera      " + options.device + " " + options.size + "@" + std::to_string(options.fps),
         {"-f", "v4l2", "-input_format", "mjpeg", "-video_size", options.size,
          "-framerate", std::to_string(options.fps), "-i", options.device}},
        {"microphone  " + options.audio,
         {"-f", "pulse", "-i", options.audio}},
    };

    int failures = 0;
    for (const Check& check : checks) {
        std::vector<std::string> args = {
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostats"};
        args.insert(args.end(), check.inputs.begin(), check.inputs.end());
        args.push_back("-t");
        args.push_back("1");
        args.push_back("-f");
        args.push_back("null");
        args.push_back("-");

        std::string stderr_text;
        const int code = run_capture(args, stderr_text, 20);
        if (code == 127) {
            std::fprintf(stderr, "ffmpeg not found — install the ffmpeg package\n");
            return 1;
        }
        if (code == -2) {
            std::printf("%s — FAIL: no data within 20 s\n", check.label.c_str());
            ++failures;
        } else if (code == 0) {
            std::printf("%s — OK\n", check.label.c_str());
        } else {
            const std::string reason = pick_error_line(stderr_text);
            const std::string detail = reason.empty()
                ? "ffmpeg exit code " + std::to_string(code)
                : reason;
            std::printf("%s — FAIL: %s\n", check.label.c_str(), detail.c_str());
            ++failures;
        }
    }
    if (failures != 0) {
        std::fflush(stdout);
        std::fprintf(stderr, "\n%d of %zu devices failed\n", failures, checks.size());
        return 1;
    }
    return 0;
}

Recorder::Recorder(RecorderOptions options) : options_(std::move(options)) {}

Recorder::~Recorder() {
    stop_blocking();
    if (stderr_thread_.joinable()) stderr_thread_.join();
    if (stop_thread_.joinable()) stop_thread_.join();
}

bool Recorder::start(std::string& error) {
    if (pid_ >= 0) {
        error = "a recording is already running";
        return false;
    }
    std::error_code ec;
    fs::create_directories(options_.out_dir, ec);
    if (ec) {
        error = "cannot create " + options_.out_dir + ": " + ec.message();
        return false;
    }

    const std::string base = timestamp_name();
    fs::path path = fs::path(options_.out_dir) / (base + ".mp4");
    int suffix = 2;
    while (fs::exists(path, ec)) {
        path = fs::path(options_.out_dir) / (base + "-" + std::to_string(suffix++) + ".mp4");
    }
    path_ = path.generic_string();

    int inpipe[2] = {-1, -1};
    int errpipe[2] = {-1, -1};
    if (::pipe(inpipe) != 0 || ::pipe(errpipe) != 0) {
        if (inpipe[0] >= 0) { ::close(inpipe[0]); ::close(inpipe[1]); }
        if (errpipe[0] >= 0) { ::close(errpipe[0]); ::close(errpipe[1]); }
        error = "cannot create pipes";
        return false;
    }

    const std::vector<std::string> args = build_ffmpeg_cmd(path_, options_);
    std::vector<char*> argv = make_argv(args);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inpipe[0]); ::close(inpipe[1]);
        ::close(errpipe[0]); ::close(errpipe[1]);
        error = "cannot fork ffmpeg";
        return false;
    }
    if (pid == 0) {
        ::close(inpipe[1]);
        ::close(errpipe[0]);
        ::dup2(inpipe[0], STDIN_FILENO);
        ::dup2(errpipe[1], STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::close(devnull);
        }
        ::close(inpipe[0]);
        ::close(errpipe[1]);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(inpipe[0]);
    ::close(errpipe[1]);
    pid_ = static_cast<int>(pid);
    stdin_fd_ = inpipe[1];
    started_at_ = monotonic_now();
    finished_ = false;
    finish_code_ = -1;
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(log_mu_);
        log_.clear();
    }
    if (stderr_thread_.joinable()) stderr_thread_.join();
    stderr_thread_ = std::thread(&Recorder::drain_stderr, this, errpipe[0]);
    return true;
}

std::optional<int> Recorder::poll() {
    if (pid_ < 0 || stopping_.load()) return std::nullopt;
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result == 0) return std::nullopt;
    if (result < 0) {
        if (errno == EINTR) return std::nullopt;
        pid_ = -1;
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
        return -1;
    }
    if (stdin_fd_ >= 0) {
        ::close(stdin_fd_);
        stdin_fd_ = -1;
    }
    pid_ = -1;
    return status_code(status);
}

void Recorder::stop_async() {
    if (pid_ < 0 || stopping_.load()) return;
    stopping_ = true;
    finished_ = false;
    if (stop_thread_.joinable()) stop_thread_.join();
    stop_thread_ = std::thread(&Recorder::stop_worker, this);
}

bool Recorder::take_finished(int& code) {
    if (!finished_.load()) return false;
    if (stop_thread_.joinable()) stop_thread_.join();
    code = finish_code_.load();
    finished_ = false;
    pid_ = -1;
    stdin_fd_ = -1;
    stopping_ = false;
    return true;
}

void Recorder::stop_blocking() {
    if (pid_ < 0) {
        if (stop_thread_.joinable()) stop_thread_.join();
        if (stderr_thread_.joinable()) stderr_thread_.join();
        return;
    }
    const int pid = pid_;
    const int fd = stdin_fd_;
    pid_ = -1;
    stdin_fd_ = -1;
    stopping_ = true;
    finish_code_ = shutdown_process(pid, fd);
    stopping_ = false;
    finished_ = true;
}

int Recorder::pid() const {
    return pid_;
}

std::string Recorder::last_error() const {
    std::lock_guard<std::mutex> lock(log_mu_);
    std::string joined;
    for (const std::string& line : log_) {
        if (!joined.empty()) joined.push_back('\n');
        joined += line;
    }
    return pick_error_line(joined);
}

void Recorder::drain_stderr(int fd) {
    char buffer[4096];
    std::string pending;
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            pending.append(buffer, static_cast<std::size_t>(n));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                while (!line.empty() &&
                       std::isspace(static_cast<unsigned char>(line.back()))) {
                    line.pop_back();
                }
                if (line.empty()) continue;
                std::lock_guard<std::mutex> lock(log_mu_);
                log_.push_back(std::move(line));
                while (log_.size() > 200) log_.pop_front();
            }
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    if (!pending.empty()) {
        std::lock_guard<std::mutex> lock(log_mu_);
        log_.push_back(std::move(pending));
        while (log_.size() > 200) log_.pop_front();
    }
    ::close(fd);
}

void Recorder::stop_worker() {
    const int pid = pid_;
    const int fd = stdin_fd_;
    const int code = shutdown_process(pid, fd);
    finish_code_ = code;
    stopping_ = false;
    finished_ = true;
}

int Recorder::shutdown_process(int pid, int stdin_fd) {
    if (pid < 0) return -1;
    if (stdin_fd >= 0) {
        const char q = 'q';
        // A broken pipe here just means ffmpeg already exited.
        const ssize_t ignored = ::write(stdin_fd, &q, 1);
        (void)ignored;
        ::close(stdin_fd);
    }
    int code = wait_with_timeout(pid, 6);
    if (code == -2) {
        ::kill(pid, SIGTERM);
        code = wait_with_timeout(pid, 3);
        if (code == -2) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            code = status_code(status);
        }
    }
    return code;
}

} // namespace tp
