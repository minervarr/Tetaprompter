#pragma once

#include <string>

namespace tp {

struct RuntimeOptions {
    std::string script;
    std::string device = "/dev/video0";
    std::string audio = "default";
    std::string size = "1280x720";
    std::string output = "DP-2"; // Wayland connector for fullscreen
    int fps = 30;
    int line_percent = 25;
    bool windowed = false;
    bool print_timing = false;
    bool words = false;
    bool check = false;
    std::string capture_path; // dev: write one frame as a PPM and exit
    double seek_seconds = -1.0; // dev: start the read at this clock
    bool demo_help = false;
    bool demo_picker = false;
    bool autoplay = false;
    double record_test_seconds = -1.0; // dev: record for this long, then exit
};

} // namespace tp
