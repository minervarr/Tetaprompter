#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tp {

struct RecorderOptions {
    std::string device = "/dev/video0";
    std::string audio = "default";
    std::string size = "1280x720";
    int fps = 30;
    int crf = 23;
    std::string out_dir = "recordings";
};

std::vector<std::string> build_ffmpeg_cmd(const std::string& out_path,
                                          const RecorderOptions& options);
std::string pick_error_line(const std::string& stderr_text, std::size_t limit = 200);
int check_devices(const RecorderOptions& options);
bool parse_capture_size(const std::string& value, int& width, int& height,
                        std::string& error);

class Recorder {
public:
    explicit Recorder(RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Creates the output directory and spawns ffmpeg. On failure fills `error`.
    bool start(std::string& error);
    bool active() const { return pid_ >= 0; }
    bool finalizing() const { return stopping_.load(); }

    // Exit status if ffmpeg died on its own, otherwise nullopt.
    std::optional<int> poll();

    // Stops without blocking the caller: a daemon thread writes 'q' and waits.
    void stop_async();
    // Non-blocking: true once the stop thread has finished, with its status.
    bool take_finished(int& code);
    void stop_blocking();

    const std::string& path() const { return path_; }
    double started_at() const { return started_at_; }
    std::string last_error() const;

    // Called from the tick loop; reaps a process that exited spontaneously.
    int pid() const;

private:
    static int shutdown_process(int pid, int stdin_fd);
    void drain_stderr(int fd);
    void stop_worker();

    RecorderOptions options_;
    std::string path_;
    double started_at_ = 0.0;
    int pid_ = -1;
    int stdin_fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> finish_code_{-1};
    std::thread stderr_thread_;
    std::thread stop_thread_;
    mutable std::mutex log_mu_;
    std::deque<std::string> log_;
};

} // namespace tp
