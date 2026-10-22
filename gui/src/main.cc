#include "app_main.hh"

#include <cstdio>
#include <cstring>

#include "app.hh"
#include "teleprompter/colour.hh"
#include "teleprompter/recorder.hh"
#include "teleprompter/runtime.hh"

namespace {

void usage() {
    std::fputs(
        "usage: teleprompter [script] [--line PCT] [--output CONNECTOR]\n"
        "                   [--font newcm|opendyslexic|PATH] [--windowed]\n"
        "       teleprompter --print-timing [--words] [script]\n"
        "       teleprompter --check\n",
        stderr);
}

} // namespace

int app_shell_main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--print-timing") == 0 || std::strcmp(arg, "--check") == 0 ||
            std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            app_shell_restore_console();
            break;
        }
    }
    tp::RuntimeOptions options;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        }
        if (std::strcmp(arg, "--windowed") == 0) {
            options.windowed = true;
            continue;
        }
        if (std::strcmp(arg, "--print-timing") == 0) {
            options.print_timing = true;
            continue;
        }
        if (std::strcmp(arg, "--words") == 0) {
            options.words = true;
            continue;
        }
        if (std::strcmp(arg, "--check") == 0) {
            options.check = true;
            continue;
        }
        if (std::strcmp(arg, "--seek") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --seek needs seconds\n");
                return 2;
            }
            options.seek_seconds = std::atof(argv[++i]);
            continue;
        }
        if (std::strcmp(arg, "--autoplay") == 0) {
            options.autoplay = true;
            continue;
        }
        if (std::strcmp(arg, "--record-test") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --record-test needs seconds\n");
                return 2;
            }
            options.record_test_seconds = std::atof(argv[++i]);
            continue;
        }
        if (std::strcmp(arg, "--demo-help") == 0) {
            options.demo_help = true;
            continue;
        }
        if (std::strcmp(arg, "--demo-picker") == 0) {
            options.demo_picker = true;
            continue;
        }
        if (std::strcmp(arg, "--demo-font") == 0) {
            options.demo_font = true;
            continue;
        }
        if (std::strcmp(arg, "--capture") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --capture needs a path\n");
                return 2;
            }
            options.capture_path = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--line") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --line needs a percentage\n");
                return 2;
            }
            options.line_percent = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(arg, "--font") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --font needs a name or path\n");
                return 2;
            }
            options.font = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --output needs a connector name\n");
                return 2;
            }
            options.output = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--device") == 0 || std::strcmp(arg, "--audio") == 0 ||
            std::strcmp(arg, "--size") == 0 || std::strcmp(arg, "--fps") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", arg);
                return 2;
            }
            const std::string value = argv[++i];
            if (std::strcmp(arg, "--device") == 0) {
                options.device = value;
            } else if (std::strcmp(arg, "--audio") == 0) {
                options.audio = value;
            } else if (std::strcmp(arg, "--size") == 0) {
                int width = 0, height = 0;
                std::string error;
                if (!tp::parse_capture_size(value, width, height, error)) {
                    std::fprintf(stderr, "error: %s\n", error.c_str());
                    return 2;
                }
                options.size = value;
            } else {
                options.fps = std::atoi(value.c_str());
                if (options.fps <= 0) {
                    std::fprintf(stderr, "error: --fps must be positive\n");
                    return 2;
                }
            }
            continue;
        }
        if (arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option: %s\n", arg);
            usage();
            return 2;
        }
        if (!options.script.empty()) {
            std::fprintf(stderr, "error: more than one script given\n");
            return 2;
        }
        options.script = arg;
    }

    if (options.print_timing) {
        const std::string path = options.script.empty() ? tp::default_script_path() : options.script;
        return tp::print_timing(path, options.words);
    }
    if (options.check) {
        tp::RecorderOptions recorder;
        recorder.device = options.device;
        recorder.audio = options.audio;
        recorder.size = options.size;
        recorder.fps = options.fps;
        return tp::check_devices(recorder);
    }

    tp::App app(options);
    if (!app.create()) {
        return 1;
    }
    app.run();
    return 0;
}
