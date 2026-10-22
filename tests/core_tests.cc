#include "teleprompter/script.hh"
#include "teleprompter/colour.hh"
#include "teleprompter/recents.hh"
#include "teleprompter/recorder.hh"
#include "teleprompter/settings.hh"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace tp;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        std::abort(); \
    } \
} while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static void strict_timing() {
    try {
        paragraph_plan("[0.4]One two.");
        CHECK(false);
    } catch (const ScriptTimingError& error) {
        CHECK(error.issues().size() == 1);
        CHECK(error.issues()[0].line == 1);
        CHECK(error.issues()[0].kind == "word");
        CHECK(error.issues()[0].text == "two.");
    }

    try {
        paragraph_plan("[0.4]One.\n\n[smile — wait]");
        CHECK(false);
    } catch (const ScriptTimingError& error) {
        CHECK(error.issues()[0].line == 3);
        CHECK(error.issues()[0].kind == "comment");
    }

    try {
        paragraph_plan("[0]One.");
        CHECK(false);
    } catch (const ScriptTimingError& error) {
        CHECK(error.issues()[0].reason.find("zero duration") != std::string::npos);
    }

    try {
        paragraph_plan("[0.2]One two three.");
        CHECK(false);
    } catch (const ScriptTimingError& error) {
        CHECK(error.issues().size() == 2);
        CHECK(error.issues()[0].text == "two");
        CHECK(error.issues()[1].text == "three.");
    }
}

static void plan_basics() {
    const auto plan = paragraph_plan("{warm}[0.45]Good [0.30]morning, [0.55]everyone!");
    CHECK(plan.size() == 1);
    const Paragraph& p = plan[0];
    CHECK(p.text == "Good morning, everyone!");
    CHECK(near(p.seconds, 1.30));
    CHECK(p.start == 0.0);
    CHECK(p.mood == "warm");
    CHECK(!p.is_direction);
    CHECK(p.words.size() == 3);
    CHECK(p.words[0].text == "Good" && near(p.words[0].seconds, 0.45));
    CHECK(p.words[1].text == "morning," && near(p.words[1].seconds, 0.30));
    CHECK(p.words[2].text == "everyone!" && near(p.words[2].seconds, 0.55));
    CHECK(near(p.words[1].clock, 0.45));
    CHECK(near(p.words[2].clock, 0.75));
    CHECK(p.words[0].char_start == 0);
    CHECK(p.words[1].char_start == 5);
    CHECK(p.words[2].char_start == 14);
    CHECK(p.words[0].byte_start == 0);
    CHECK(p.words[1].byte_start == 5);
    CHECK(p.words[2].byte_start == 14);

    const auto two = paragraph_plan("[0.4]One [0.6]two.\n\n[0.8]Three.");
    CHECK(two.size() == 2);
    CHECK(near(two[0].start, 0.0) && near(two[1].start, 1.0));
    CHECK(near(two[0].seconds, 1.0) && near(two[1].seconds, 0.8));
}

static void comments_and_sections() {
    const auto plan = paragraph_plan(
        "[0.4]Before.\n\n[1.5] [look at the camera]\n\n[2.0]\n\n[0.3]After.");
    CHECK(plan.size() == 4);
    CHECK(plan[1].is_direction);
    CHECK(plan[1].text == "[look at the camera]");
    CHECK(near(plan[1].seconds, 1.5));
    CHECK(plan[2].is_direction);
    CHECK(plan[2].text == " ");
    CHECK(near(plan[2].seconds, 2.0));
    CHECK(near(plan[3].start, 3.9));

    auto state = section_state(plan, 0.5);
    CHECK(state.has_value());
    CHECK(state->index == 1);
    CHECK(near(state->remaining, 1.4));
    // The live plan's starts are used when not supplied.
    auto mid = section_state(plan, 1.15);
    CHECK(mid.has_value());
    CHECK(near(mid->elapsed, 0.75));
    CHECK(near(mid->progress, 0.5));
    CHECK(section_title(plan[2]) == "Hold");
    CHECK(!section_state(plan, 4.0).has_value());
}

static void moods_and_markup() {
    const auto plan = paragraph_plan(
        "{warm: Opening}[0.1]One.\n\n[0.2]Two.\n\n{somber}[0.3]Three.");
    CHECK(plan[0].theme == "Opening");
    CHECK(plan[1].theme == "Opening");
    CHECK(plan[1].mood == "warm");
    CHECK(plan[2].mood == "somber");

    const auto styled = paragraph_plan("[0.4]**Bold** [0.3]*Italic*")[0];
    CHECK(styled.text == "Bold Italic");
    CHECK(styled.words[0].bold);
    CHECK(styled.words[1].italic);
    CHECK(styled.markup == "<b>Bold</b> <i>Italic</i>");
    CHECK(to_markup("a < b & c") == "a &lt; b &amp; c");
}

static void utf8_offsets() {
    const auto p = paragraph_plan("[0.3]café [0.2]— [0.3]lens.")[0];
    CHECK(p.words[0].char_start == 0 && p.words[0].byte_start == 0);
    CHECK(p.words[1].char_start == 5 && p.words[1].byte_start == 6);
    CHECK(p.words[2].char_start == 7 && p.words[2].byte_start == 10);
}

static void timeline_math() {
    Timeline timeline;
    timeline.rebuild_entries({{0.0, 1.0}, {10.0, 1.0}, {10.0, 2.0}, {20.0, 1.0}}, 20.0);
    CHECK(timeline.knots().size() == 5);
    CHECK(near(timeline.total_weight(), 5.0));
    CHECK(near(timeline.pos_from_weight(0.5), 0.0));
    CHECK(near(timeline.pos_from_weight(1.5), 5.0));
    CHECK(near(timeline.pos_from_weight(2.0), 10.0));
    CHECK(near(timeline.weight_from_pos(5.0), 1.5));
    CHECK(near(timeline.weight_from_pos(10.0), 2.0));
    CHECK(near(timeline.weight_from_pos(15.0), 4.5));

    Timeline clamped;
    clamped.rebuild_entries({{0.0, 1.0}, {50.0, 1.0}, {10.0, 1.0}}, 30.0);
    CHECK(clamped.knots().size() == 4);
    CHECK(near(clamped.knots()[2], 30.0));
    CHECK(near(clamped.knots()[3], 30.0));
}

static void sample_score() {
    CHECK(script_read_time(sample_script_text()).has_value());
    CHECK(*script_read_time(sample_script_text()) == "0:23");
    CHECK(!script_read_time("[1.0]Hello. [smile]").has_value());
    CHECK(script_read_time("") == std::string("0:00"));
}

static void formatting() {
    CHECK(format_time(0) == "0:00");
    CHECK(format_time(59.6) == "1:00");
    CHECK(format_time(119.4) == "1:59");
    CHECK(format_time(-3) == "0:00");
    CHECK(human_size(999) == "999 B");
    CHECK(human_size(2048) == "2.0 KB");
    CHECK(near(word_fill(2.0, 1.5, 2.75), 0.5));
    CHECK(near(character_fill(0.375, 0, 4), 1.0));
    CHECK(near(character_fill(0.375, 1, 4), 0.5));
    CHECK(near(character_fill(0.375, 2, 4), 0.0));
    CHECK(format_age(0.0, 1000.0) == "unknown");
    CHECK(format_age(-1.0, 1000.0) == "unknown");
}

static void colour_and_truncation() {
    const Rgb fallback{1, 2, 3};
    CHECK(hex_from_rgb(parse_hex("#zzzzzz", fallback)) == "#010203");
    CHECK(hex_from_rgb(parse_hex("#F0a03C")) == "#f0a03c");
    CHECK(hex_from_rgb(mood_colour("WARM")) == "#f5dcc0");
    CHECK(hex_from_rgb(mood_colour("#102030")) == "#102030");
    CHECK(hex_from_rgb(mood_colour("not-a-mood")) == "#f2f2f2");

    CHECK(preview_text("abcdef", 4) == "abc…");
    CHECK(preview_text("αβγδε", 4) == "αβγ…");
    CHECK(preview_text("αβ", 4) == "αβ");
    CHECK(preview_text("abc", 0) == "…");

    Paragraph paragraph;
    paragraph.text = "[αβγδε]";
    CHECK(section_title(paragraph, 3) == "αβ…");
    CHECK(section_title(paragraph, 20) == "αβγδε");
}

static void oversized_duration_is_clamped() {
    const std::string huge = "[" + std::string(400, '9') + "]Hello.";
    const auto plan = paragraph_plan(huge);
    CHECK(plan.size() == 1);
    CHECK(near(plan[0].words[0].seconds, kWordMaxSeconds));
}

static void recorder_helpers() {
    RecorderOptions options;
    const auto command = build_ffmpeg_cmd("/tmp/out.mp4", options);
    CHECK(!command.empty() && command.front() == "ffmpeg");
    CHECK(std::find(command.begin(), command.end(), "1280x720") != command.end());
    CHECK(std::find(command.begin(), command.end(), "/dev/video0") != command.end());
    CHECK(command.back() == "/tmp/out.mp4");

    int width = 0;
    int height = 0;
    std::string error;
    CHECK(parse_capture_size("1280x720", width, height, error));
    CHECK(width == 1280 && height == 720);
    CHECK(!parse_capture_size("1281x720", width, height, error));
    CHECK(!parse_capture_size("1280X720", width, height, error));

    CHECK(pick_error_line("[in#0 @ 0x1] Permission denied\n") == "Permission denied");
    CHECK(pick_error_line(" \n\t\n").empty());
}

static void settings_round_trip() {
    const std::string dir = "/tmp/teleprompter-vk-settings";
    std::filesystem::create_directories(dir);
    const char* old_state = std::getenv("XDG_STATE_HOME");
    const std::string old_value = old_state != nullptr ? old_state : "";
    ::setenv("XDG_STATE_HOME", dir.c_str(), 1);
    CHECK(load_font_setting().empty());
    CHECK(save_font_setting("opendyslexic"));
    CHECK(load_font_setting() == "opendyslexic");
    CHECK(save_font_setting("newcm"));
    CHECK(load_font_setting() == "newcm");
    std::filesystem::remove(settings_path());
    CHECK(load_font_setting().empty());
    if (old_state != nullptr) ::setenv("XDG_STATE_HOME", old_value.c_str(), 1);
    else ::unsetenv("XDG_STATE_HOME");
}

static void recents_round_trip() {
    const std::string dir = "/tmp/teleprompter-vk-test";
    std::filesystem::create_directories(dir);
    const std::string one = dir + "/one.txt";
    const std::string two = dir + "/two.txt";
    { std::ofstream(one) << "[0.5]One."; }
    { std::ofstream(two) << "[0.5]Two."; }
    const std::string cache = dir + "/recents.json";
    const double now = static_cast<double>(std::time(nullptr)) + 0.125;
    std::vector<Recent> entries;
    entries = remember_recent(entries, one, now - 100.0);
    entries = remember_recent(entries, two, now);
    CHECK(save_recents(entries, cache));
    const auto loaded = load_recents(cache);
    CHECK(loaded.size() == 2);
    CHECK(loaded[0].path == resolve_path(two));
    CHECK(loaded[1].path == resolve_path(one));
    CHECK(near(loaded[0].opened, now, 1e-6));
    CHECK(near(loaded[1].opened, now - 100.0, 1e-6));
    CHECK(read_script_time(one).has_value());
    CHECK(!read_script_time(dir + "/missing.txt").has_value());
}

int main() {
    strict_timing();
    plan_basics();
    comments_and_sections();
    moods_and_markup();
    utf8_offsets();
    timeline_math();
    formatting();
    colour_and_truncation();
    oversized_duration_is_clamped();
    recorder_helpers();
    sample_score();
    settings_round_trip();
    recents_round_trip();
    std::puts("teleprompter_core_tests: OK");
    return 0;
}
