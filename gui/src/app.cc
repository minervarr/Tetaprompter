#include "app.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#include "canvas.hh"
#include "host.hh"
#include "keys.hh"
#include "renderer.hh"
#include "raster_font.hh"
#include "teleprompter/colour.hh"
#include "teleprompter/settings.hh"
#include "teleprompter/script.hh"
#include "fonts.hh"

namespace tp {
namespace {

using gui::to_color;

namespace fs = std::filesystem;

::Color rgb(Rgb c, float alpha = 1.0f) {
    return to_color(c, alpha);
}

const Rgb kBg = parse_hex("#000000");
const Rgb kLibraryBg = parse_hex("#0a0a0c");
const Rgb kPanel = parse_hex("#151518");
const Rgb kText = parse_hex("#f2f2f2");
const Rgb kDim = parse_hex("#8a93a0");
const Rgb kFaint = parse_hex("#5f6874");
const Rgb kAmber = parse_hex("#f0a03c");
const Rgb kAmberLit = parse_hex("#ffb45c");
const Rgb kRed = parse_hex("#ff5f56");
const Rgb kAmberComment = parse_hex("#f0c27a");
const Rgb kKaraokeGrayRgb = parse_hex(tp::kKaraokeGray);
const Rgb kBorder = parse_hex("#3a3a42");
const Rgb kCardBorder = parse_hex("#2a2a30");
const Rgb kSoft = parse_hex("#cfcfcf");
const Rgb kStateText = parse_hex("#9aa4b2");
const Rgb kThemeText = parse_hex("#b99a6a");
const Rgb kErrorText = parse_hex("#ff8a80");
const Rgb kButtonInk = parse_hex("#141414");
const Rgb kSelectedBg = parse_hex("#2a2114");
const Rgb kSamplePanel = parse_hex("#17161a");
const Rgb kSampleBorder = parse_hex("#5a4327");
const Rgb kSampleBadge = parse_hex("#3a2a14");
const Rgb kModalBg = parse_hex("#050506");
const Rgb kModalPanel = parse_hex("#18181b");

constexpr float kHudHeight = 46.0f;
constexpr float kHudMargin = 12.0f;
constexpr float kRadiusSmall = 8.0f;
constexpr float kRadiusControl = 12.0f;
constexpr float kRadiusPanel = 16.0f;
constexpr float kCueStep = 0.02f;
constexpr float kCueMin = 0.05f;
constexpr float kCueMax = 0.60f;
constexpr float kCuePresets[] = {0.15f, 0.25f, 0.35f, 0.50f};
constexpr double kHudDimAfter = 3.0;
constexpr int kPickRow = 30;

const std::vector<std::pair<std::string, std::string>>& key_help() {
    static const std::vector<std::pair<std::string, std::string>> rows = {
        {"Space", "Start / pause scrolling — at the end, restart from the top"},
        {"R", "Start / stop recording"},
        {"Ctrl+R", "Return to the script library (refresh it there)"},
        {"Ctrl+O", "Open another script from disk"},
        {"F or F11", "Toggle fullscreen — Esc leaves fullscreen"},
        {"↑ / ↓", "Move the reading line up / down by 2 %"},
        {"C", "Cycle the reading line (15 · 25 · 35 · 50 %)"},
        {"A", "Toggle centred / flush-left text"},
        {"K", "Toggle word-by-word karaoke fill"},
        {"M", "Toggle mood colours"},
        {"T", "Choose the script font"},
        {"← / →", "Nudge the text"},
        {"Home / End", "Jump to the top / bottom"},
        {"F5 or L", "Reload the script file"},
        {"H", "Show / hide the toolbar and section timer"},
        {"? or F1", "Show this help"},
        {"Ctrl+Q", "Quit — finalizes any recording first"},
    };
    return rows;
}

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

std::string stem_of(const std::string& path) {
    return fs::path(path).stem().string();
}

std::string parent_of(const std::string& path) {
    return fs::path(path).parent_path().string();
}

std::string font_setting_value(const gui::FontPreset& preset) {
    // Keep the original directory for custom face collections so bold/italic
    // faces are rediscovered on the next launch. A single file is also valid.
    if (preset.id.rfind("custom:", 0) == 0) return preset.id.substr(7);
    return preset.id;
}

// Truncate at a UTF-8 boundary and add an ellipsis, measured with the actual
// selected face. Used where fixed-width labels share a row.
std::string fit_text(const gui::TextPainter& painter, std::string_view text,
                     float max_width, float size, FontStyle style) {
    if (text.empty() || max_width <= 0.0f) return {};
    if (painter.width(text, size, style) <= max_width) return std::string(text);
    constexpr std::string_view kEllipsis = "\u2026";
    const float ellipsis_width = painter.width(kEllipsis, size, style);
    if (ellipsis_width > max_width) return {};
    const float budget = max_width - ellipsis_width;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t begin = i;
        ++i;
        while (i < text.size() &&
               (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            ++i;
        }
        std::string candidate = out;
        candidate.append(text.substr(begin, i - begin));
        if (painter.width(candidate, size, style) > budget) break;
        out = std::move(candidate);
    }
    out += kEllipsis;
    return out;
}

bool is_script_file(const fs::path& path) {
    if (!path.has_extension()) return false;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".txt" || ext == ".md" || ext == ".text";
}

} // namespace

App::App(RuntimeOptions options) : options_(std::move(options)) {}

App::~App() = default;

double App::now_monotonic() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double App::now_epoch() const {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

bool App::create() {
    cue_fraction_ = clampf(static_cast<float>(options_.line_percent) / 100.0f, kCueMin, kCueMax);
    if (!create_host()) return false;
    fullscreen_ = !options_.windowed;
    compute_font();

    const bool explicit_font = !options_.font.empty();
    bool requested_font_loaded = false;
    std::string requested_font = options_.font;
    if (requested_font.empty()) requested_font = load_font_setting();
    if (requested_font.empty()) requested_font = "newcm";
    gui::FontPreset font_preset;
    std::string font_choice_error;
    if (gui::make_font_preset(requested_font, font_preset, font_choice_error)) {
        requested_font_loaded = true;
    } else {
        startup_font_error_ = font_choice_error + " — using New Computer Modern";
        font_preset = *gui::find_font_preset("newcm");
    }

    std::string font_error;
    if (!text_.load(*host_, font_preset, font_error)) {
        // A missing/broken chosen face must not prevent the app from opening.
        requested_font_loaded = false;
        startup_font_error_ = font_error + " — using New Computer Modern";
        font_preset = *gui::find_font_preset("newcm");
        if (!text_.load(*host_, font_preset, font_error)) {
            host_->showErrorMessage("Font error", font_error);
            std::fprintf(stderr, "error: %s\n", font_error.c_str());
            return false;
        }
    }
    current_font_ = std::move(font_preset);
    if (explicit_font && requested_font_loaded) {
        save_font_setting(font_setting_value(current_font_));
    }
    if (!create_renderer()) return false;

    RecorderOptions recorder_options;
    recorder_options.device = options_.device;
    recorder_options.audio = options_.audio;
    recorder_options.size = options_.size;
    recorder_options.fps = options_.fps;
    recorder_options.out_dir =
        (fs::path(host_->exeDir()) / "recordings").generic_string();
    recorder_ = std::make_unique<Recorder>(std::move(recorder_options));

    seed_recents();
    refresh_library();

    host_->startTimer(1, 16);
    host_->showWindow();
    needs_redraw_ = true;

    if (!options_.script.empty()) {
        open_script(options_.script, false);
    }
    if (!startup_font_error_.empty()) show_message(startup_font_error_, true, 10.0);
    if (options_.demo_help) help_visible_ = true;
    if (options_.demo_picker) open_picker();
    if (options_.demo_font) open_font_menu();
    if (options_.record_test_seconds > 0.0) {
        record_test_ = true;
        start_recording();
        record_test_stop_ = now_monotonic() + options_.record_test_seconds;
    } else if (options_.autoplay) {
        toggle_play();
    }
    return true;
}

bool App::create_host() {
    host_ = make_host();
    if (!host_) {
        std::fprintf(stderr, "error: no window host available on this platform\n");
        return false;
    }
    WindowOptions window;
    window.width = 1180;
    window.height = 760;
    // Fullscreen by default. An empty output lets the compositor place the
    // window on its current monitor; --output pins a specific connector. A
    // windowed Wayland surface cannot choose its own monitor, so --windowed
    // leaves placement to the compositor.
    window.fullscreen = !options_.windowed;
    window.output = options_.output;
    host_->setWindowOptions(window);
    if (!host_->init(this)) {
        std::fprintf(stderr, "error: could not create the window\n");
        return false;
    }
    return true;
}

bool App::create_renderer() {
    try {
        renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader(),
                                               2, OutputTarget::SdrSrgb, PresentPolicy::Vsync);
    } catch (const std::exception& error) {
        host_->showErrorMessage("Vulkan initialization failed", error.what());
        std::fprintf(stderr, "error: Vulkan initialization failed: %s\n", error.what());
        return false;
    }
    msdf_initialized_ = false;
    return true;
}

void App::compute_font() {
    const MonitorInfo monitor = host_->primaryMonitor();
    const float height = static_cast<float>(monitor.bounds.bottom - monitor.bounds.top);
    const float basis = height > 0.0f ? height : 1200.0f;
    font_px_ = clampf(std::round(basis * 0.055f), 28.0f, 120.0f);
    script_px_ = font_px_;
}

void App::seed_recents() {
    std::error_code ec;
    if (fs::exists(fs::path(recents_path()), ec)) {
        recents_ = load_recents();
        return;
    }
    recents_.clear();
    const fs::path beside = fs::path(host_->exeDir()) / "script.txt";
    if (fs::is_regular_file(beside, ec)) {
        recents_ = remember_recent({}, beside.generic_string());
        save_recents(recents_);
    }
}

void App::refresh_library() {
    recents_ = prune_missing(std::move(recents_));
    library_entries_.clear();
    const double now = now_epoch();
    for (const Recent& recent : recents_) {
        LibraryEntry entry;
        entry.path = recent.path;
        entry.title = stem_of(recent.path);
        if (entry.title.empty()) entry.title = fs::path(recent.path).filename().string();
        entry.subtitle = display_path(parent_of(recent.path));
        const std::optional<std::string> read_time = read_script_time(recent.path);
        entry.valid = read_time.has_value();
        entry.meta = (read_time ? *read_time : std::string("needs timing")) +
                     " · Opened " + format_age(recent.opened, now);
        library_entries_.push_back(std::move(entry));
    }
    LibraryEntry sample;
    sample.sample = true;
    sample.title = "Test script";
    sample.subtitle = "Built-in demo score · read-only";
    const std::optional<std::string> sample_time = script_read_time(sample_script_text());
    sample.meta = (sample_time ? *sample_time : std::string("needs timing")) +
                  " · Always available";
    sample.valid = sample_time.has_value();
    library_entries_.push_back(std::move(sample));

    if (library_selected_ < 0) library_selected_ = 0;
    if (library_selected_ >= static_cast<int>(library_entries_.size())) {
        library_selected_ = std::max(0, static_cast<int>(library_entries_.size()) - 1);
    }
    library_reveal_ = true;
    text_dirty_ = true;
    needs_redraw_ = true;
}

void App::open_picker() {
    if (rec_phase_ != RecPhase::Idle) {
        show_message("Stop recording before opening another script", true);
        return;
    }
    if (playing_) toggle_play();
    std::string start;
    if (script_path_) {
        start = parent_of(*script_path_);
    } else if (!recents_.empty()) {
        start = parent_of(recents_.front().path);
    } else {
        const char* home = std::getenv("HOME");
        start = home != nullptr ? home : ".";
    }
    std::error_code ec;
    if (!fs::is_directory(start, ec)) start = ".";
    picker_dir_ = fs::absolute(start, ec).generic_string();
    picker_open_ = true;
    picker_selected_ = 0;
    picker_scroll_ = 0;
    load_picker();
    needs_redraw_ = true;
}

void App::open_font_menu() {
    if (rec_phase_ != RecPhase::Idle) {
        show_message("Stop recording before changing fonts", true);
        return;
    }
    if (playing_) toggle_play();
    help_visible_ = false;
    picker_open_ = false;
    font_menu_open_ = true;
    font_menu_selected_ = 0;
    const auto& presets = gui::font_presets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].id == current_font_.id) font_menu_selected_ = static_cast<int>(i);
    }
    text_dirty_ = true;
    needs_redraw_ = true;
}

void App::apply_font(const gui::FontPreset& preset, bool persist, bool announce) {
    std::string error;
    if (!text_.reload(*host_, preset, error)) {
        show_message("Font error: " + error, true);
        return;
    }
    current_font_ = preset;
    if (persist) save_font_setting(font_setting_value(preset));
    msdf_initialized_ = false;
    font_dirty_ = true;
    text_dirty_ = true;
    layout_dirty_ = true;
    needs_redraw_ = true;
    if (announce) show_message("Font: " + preset.label);
}

void App::load_picker() {
    picker_entries_.clear();
    std::error_code ec;
    const fs::path dir(picker_dir_);
    const fs::path parent = dir.parent_path();
    if (!parent.empty() && parent != dir) {
        picker_entries_.push_back(PickerEntry{"..", parent.generic_string(), true});
    }
    std::vector<PickerEntry> directories;
    std::vector<PickerEntry> files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        const std::string name = path.filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            directories.push_back(PickerEntry{name + "/", path.generic_string(), true});
        } else if (it->is_regular_file(type_ec) && is_script_file(path)) {
            files.push_back(PickerEntry{name, path.generic_string(), false});
        }
    }
    auto by_name = [](const PickerEntry& a, const PickerEntry& b) { return a.name < b.name; };
    std::sort(directories.begin(), directories.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    for (const PickerEntry& entry : directories) picker_entries_.push_back(entry);
    for (const PickerEntry& entry : files) picker_entries_.push_back(entry);
    if (picker_selected_ >= static_cast<int>(picker_entries_.size())) {
        picker_selected_ = std::max(0, static_cast<int>(picker_entries_.size()) - 1);
    }
    text_dirty_ = true;
    needs_redraw_ = true;
}

void App::picker_activate(int index) {
    if (index < 0 || index >= static_cast<int>(picker_entries_.size())) return;
    const PickerEntry entry = picker_entries_[static_cast<std::size_t>(index)];
    if (entry.directory) {
        picker_dir_ = entry.path;
        picker_selected_ = 0;
        picker_scroll_ = 0;
        load_picker();
        return;
    }
    picker_open_ = false;
    open_script(entry.path, true);
}

void App::picker_up() {
    std::error_code ec;
    const fs::path parent = fs::path(picker_dir_).parent_path();
    if (parent.empty() || parent == fs::path(picker_dir_)) return;
    picker_dir_ = parent.generic_string();
    picker_selected_ = 0;
    picker_scroll_ = 0;
    load_picker();
}

void App::open_script(const std::string& path, bool remember) {
    if (rec_phase_ != RecPhase::Idle) {
        show_message("Stop recording before changing scripts", true);
        return;
    }
    const std::string resolved = resolve_path(path);
    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        show_message("Script not found: " + resolved, true);
        recents_ = prune_missing(std::move(recents_));
        refresh_library();
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();

    std::vector<std::string> warnings;
    std::vector<Paragraph> plan;
    try {
        plan = paragraph_plan(content, &warnings);
    } catch (const ScriptTimingError& error) {
        show_message(fs::path(resolved).filename().string() + ": " + error.summary(), true);
        return;
    }

    script_path_ = resolved;
    is_sample_ = false;
    page_ = Page::Prompter;
    apply_plan(std::move(plan), std::move(warnings));
    help_visible_ = false;
    if (!options_.windowed) {
        fullscreen_ = true;
        host_->setFullscreen(true);
    }
    if (remember) {
        recents_ = remember_recent(std::move(recents_), resolved);
        save_recents(recents_);
        refresh_library();
    }
    needs_redraw_ = true;
    update_inhibit();
}

void App::open_test_script() {
    if (rec_phase_ != RecPhase::Idle) {
        show_message("Stop recording before changing scripts", true);
        return;
    }
    const std::string content = sample_script_text();
    if (content.empty()) {
        show_message("The built-in test script is unavailable", true);
        return;
    }
    std::vector<std::string> warnings;
    std::vector<Paragraph> plan;
    try {
        plan = paragraph_plan(content, &warnings);
    } catch (const ScriptTimingError& error) {
        show_message("Test script: " + error.summary(), true);
        return;
    }
    script_path_.reset();
    is_sample_ = true;
    page_ = Page::Prompter;
    apply_plan(std::move(plan), std::move(warnings));
    help_visible_ = false;
    if (!options_.windowed) {
        fullscreen_ = true;
        host_->setFullscreen(true);
    }
    needs_redraw_ = true;
    update_inhibit();
}

void App::reload_script(bool notify) {
    std::string content;
    if (is_sample_) {
        content = sample_script_text();
    } else if (script_path_) {
        std::ifstream in(*script_path_, std::ios::binary);
        if (!in) {
            show_message("Cannot read " + *script_path_, true);
            return;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        content = buffer.str();
    } else {
        show_message("No script selected", true);
        return;
    }
    std::vector<std::string> warnings;
    std::vector<Paragraph> plan;
    try {
        plan = paragraph_plan(content, &warnings);
    } catch (const ScriptTimingError& error) {
        show_message(error.summary(), true, 10.0);
        return;
    }
    apply_plan(std::move(plan), std::move(warnings));
    if (notify) {
        int words = 0;
        int comments = 0;
        for (const Paragraph& paragraph : plan_) {
            for (const Word& word : paragraph.words) {
                if (word.is_comment) ++comments;
                else if (!word.text.empty()) ++words;
            }
        }
        const std::string name = is_sample_
            ? std::string("Test script")
            : (script_path_ ? fs::path(*script_path_).filename().string()
                            : std::string("script"));
        show_message("Reloaded " + name +
                     " (" + std::to_string(words) + " words, " +
                     std::to_string(comments) + " comments, " +
                     format_time(duration_) + ")");
    }
}

void App::go_home() {
    if (rec_phase_ != RecPhase::Idle) {
        show_message("Stop recording before returning to the library", true);
        return;
    }
    if (playing_) toggle_play();
    help_visible_ = false;
    picker_open_ = false;
    page_ = Page::Library;
    fullscreen_ = false;
    host_->setFullscreen(false);
    recents_ = prune_missing(std::move(recents_));
    refresh_library();
    needs_redraw_ = true;
    update_inhibit();
}

void App::apply_plan(std::vector<Paragraph> plan, std::vector<std::string> warnings) {
    plan_ = std::move(plan);
    flat_words_.clear();
    paragraph_starts_.clear();
    duration_ = 0.0;
    for (const Paragraph& paragraph : plan_) {
        paragraph_starts_.push_back(paragraph.start);
        duration_ += paragraph.seconds;
        for (const Word& word : paragraph.words) flat_words_.push_back(word);
    }
    total_weight_ = duration_;
    timeline_.clear(total_weight_);
    w_ = 0.0;
    anchor_w_ = 0.0;
    anchor_t_ = now_monotonic();
    scroll_ = 0.0;
    playing_ = false;
    at_end_ = false;
    layout_dirty_ = true;
    needs_redraw_ = true;
    hud_visible_ = true;
    hud_dimmed_ = false;
    last_motion_t_ = now_monotonic();
    if (!warnings.empty()) {
        std::string text = warnings.front();
        if (warnings.size() > 1) text += " (+" + std::to_string(warnings.size() - 1) + " more)";
        show_message(text, true, 8.0);
    }
    text_dirty_ = true;
}

void App::reset_playback() {
    w_ = 0.0;
    scroll_ = 0.0;
    anchor_w_ = 0.0;
    anchor_t_ = now_monotonic();
    playing_ = false;
    at_end_ = false;
    needs_redraw_ = true;
}

void App::rebuild_layout() {
    if (!renderer_ || plan_.empty()) {
        layout_ = gui::ScriptLayout{};
        return;
    }
    gui::ScriptMetrics metrics;
    metrics.width = static_cast<float>(renderer_->width());
    metrics.font_px = script_px_ * current_font_.size_scale;
    metrics.centered = centered_;
    layout_ = gui::build_script_layout(plan_, text_, metrics);
    layout_width_ = renderer_->width();
    layout_height_ = renderer_->height();
}

void App::rebuild_timeline() {
    if (layout_.words.empty()) {
        timeline_.clear(duration_);
        total_weight_ = duration_;
        scroll_ = 0.0;
        return;
    }
    std::vector<std::pair<double, double>> entries;
    entries.reserve(layout_.word_center.size());
    for (std::size_t i = 0; i < layout_.word_center.size() && i < flat_words_.size(); ++i) {
        entries.emplace_back(static_cast<double>(layout_.word_center[i]),
                             flat_words_[i].seconds);
    }
    timeline_.rebuild_entries(entries, static_cast<double>(layout_.max_scroll));
    total_weight_ = timeline_.total_weight();
    if (total_weight_ <= 0.0) total_weight_ = duration_;
    if (options_.seek_seconds >= 0.0) {
        w_ = std::min(options_.seek_seconds, total_weight_);
        options_.seek_seconds = -1.0;
    }
    w_ = std::min(w_, total_weight_);
    scroll_ = timeline_.pos_from_weight(w_);
}

void App::update_inhibit() {
    if (host_) host_->setKeepAwake(playing_ || rec_phase_ != RecPhase::Idle);
}

void App::show_message(const std::string& text, bool error, double seconds) {
    message_ = text;
    message_error_ = error;
    message_until_ = now_monotonic() + seconds;
    text_dirty_ = true;
    needs_redraw_ = true;
}

bool App::message_visible() const {
    return !message_.empty() && now_monotonic() < message_until_;
}

std::string App::state_text() const {
    if (plan_.empty()) return "No script";
    if (playing_) return "Playing";
    if (at_end_) return "Done";
    if (w_ > 1e-6) return "Paused";
    return "Ready";
}

std::string App::remaining_text() const {
    if (total_weight_ <= 0.0 || (!playing_ && scroll_ < 0.5 && !at_end_)) return {};
    const double remaining = std::max(0.0, (1.0 - w_ / total_weight_) * duration_);
    return format_time(remaining) + " left";
}

std::string App::theme_text() const {
    if (flat_words_.empty()) return {};
    int index = static_cast<int>(std::upper_bound(
        flat_words_.begin(), flat_words_.end(), w_,
        [](double clock, const Word& word) { return clock < word.clock; }) - flat_words_.begin()) - 1;
    index = std::max(0, std::min(index, static_cast<int>(flat_words_.size()) - 1));
    const Word& word = flat_words_[static_cast<std::size_t>(index)];
    if (word.theme.empty()) return {};
    double end = total_weight_;
    for (std::size_t i = static_cast<std::size_t>(index); i < flat_words_.size(); ++i) {
        if (flat_words_[i].theme != word.theme) {
            end = flat_words_[i].clock;
            break;
        }
    }
    return word.mood + " · " + word.theme + " · " + format_time(std::max(0.0, end - w_));
}

void App::toggle_play() {
    const double now = now_monotonic();
    if (playing_) {
        w_ = anchor_w_ + (now - anchor_t_);
        playing_ = false;
        anchor_w_ = w_;
        anchor_t_ = now;
        hud_dimmed_ = false;
        update_inhibit();
        needs_redraw_ = true;
        return;
    }
    if (plan_.empty()) {
        show_message("No script loaded");
        return;
    }
    if (renderer_ && (layout_dirty_ ||
                      renderer_->width() != layout_width_ ||
                      renderer_->height() != layout_height_)) {
        refresh_text_font();
        rebuild_layout();
        rebuild_timeline();
        layout_dirty_ = false;
    }
    if (timeline_.knots().size() < 2 || layout_.max_scroll <= 0.0f) {
        show_message("Text fits on screen — nothing to scroll");
        return;
    }
    if (at_end_ || w_ >= total_weight_ - 1e-6) {
        w_ = 0.0;
        scroll_ = 0.0;
        at_end_ = false;
    }
    playing_ = true;
    anchor_w_ = w_;
    anchor_t_ = now;
    last_motion_t_ = now;
    hud_dimmed_ = false;
    update_inhibit();
    needs_redraw_ = true;
}

void App::seek(double position) {
    scroll_ = std::max(0.0, std::min(position, static_cast<double>(layout_.max_scroll)));
    w_ = timeline_.weight_from_pos(scroll_);
    anchor_w_ = w_;
    anchor_t_ = now_monotonic();
    needs_redraw_ = true;
}

void App::nudge(double delta) {
    seek(scroll_ + delta);
}

void App::cycle_cue() {
    float next = kCuePresets[0];
    for (float preset : kCuePresets) {
        if (preset > cue_fraction_ + 1e-4f) {
            next = preset;
            break;
        }
    }
    cue_fraction_ = next;
    needs_redraw_ = true;
    show_message("Reading line: " + std::to_string(static_cast<int>(std::lround(cue_fraction_ * 100))) + "%");
}

void App::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    host_->setFullscreen(fullscreen_);
    needs_redraw_ = true;
}

void App::start_recording() {
    if (rec_phase_ == RecPhase::Finalizing) {
        show_message("Finalizing the previous take…");
        return;
    }
    std::string error;
    if (!recorder_->start(error)) {
        show_message("Recording failed: " + error, true);
        return;
    }
    rec_phase_ = RecPhase::Recording;
    rec_started_ = now_monotonic();
    show_message("Recording → " + fs::path(recorder_->path()).filename().string());
    update_inhibit();
}

void App::stop_recording() {
    if (rec_phase_ != RecPhase::Recording) return;
    rec_phase_ = RecPhase::Finalizing;
    recorder_->stop_async();
    show_message("Finalizing the take…");
    update_inhibit();
}

void App::finish_recording(int code) {
    rec_phase_ = RecPhase::Idle;
    std::error_code ec;
    const auto size = fs::file_size(recorder_->path(), ec);
    if (code == 0 && !ec && size > 0) {
        show_message("Saved " + fs::path(recorder_->path()).filename().string() +
                     " (" + human_size(static_cast<std::uint64_t>(size)) + ")");
    } else {
        const std::string detail = recorder_->last_error();
        show_message("Recording failed: " + (detail.empty()
            ? "ffmpeg exit code " + std::to_string(code) : detail), true);
    }
    update_inhibit();
    if (record_test_ && rec_phase_ == RecPhase::Idle) {
        record_test_ = false;
        quit_ = true;
    }
}

void App::toggle_recording() {
    if (rec_phase_ == RecPhase::Recording) {
        stop_recording();
    } else if (rec_phase_ == RecPhase::Finalizing) {
        show_message("Finalizing the previous take…");
    } else {
        start_recording();
    }
    needs_redraw_ = true;
}

void App::tick() {
    const double now = now_monotonic();
    const bool message_shown = message_visible();
    if (message_shown != message_shown_last_) {
        message_shown_last_ = message_shown;
        needs_redraw_ = true;
    }

    if (rec_phase_ == RecPhase::Recording) {
        if (record_test_ && record_test_stop_ > 0.0 && now >= record_test_stop_) {
            record_test_stop_ = -1.0;
            stop_recording();
        }
        if (const std::optional<int> code = recorder_->poll()) {
            finish_recording(*code);
        }
        needs_redraw_ = true;
    } else if (rec_phase_ == RecPhase::Finalizing) {
        int code = -1;
        if (recorder_->take_finished(code)) finish_recording(code);
        needs_redraw_ = true;
    }

    if (message_visible()) needs_redraw_ = true;

    if (!is_prompter() || renderer_ == nullptr) {
        if (message_visible() || rec_phase_ != RecPhase::Idle) needs_redraw_ = true;
        if (!options_.capture_path.empty() && frames_drawn_ < 6) needs_redraw_ = true;
        return;
    }

    if (renderer_->width() != layout_width_ || renderer_->height() != layout_height_) {
        layout_dirty_ = true;
    }
    if (layout_dirty_) {
        refresh_text_font();
        rebuild_layout();
        rebuild_timeline();
        layout_dirty_ = false;
        needs_redraw_ = true;
    }

    if (playing_) {
        w_ = anchor_w_ + (now - anchor_t_);
        if (total_weight_ > 0.0 && w_ >= total_weight_) {
            w_ = total_weight_;
            playing_ = false;
            at_end_ = true;
            hud_dimmed_ = false;
            show_message("Done");
            update_inhibit();
        }
        scroll_ = timeline_.pos_from_weight(w_);
        if (playing_ && now - last_motion_t_ > kHudDimAfter) {
            if (!hud_dimmed_) {
                hud_dimmed_ = true;
            }
        }
        needs_redraw_ = true;
    } else {
        if (hud_dimmed_) {
            hud_dimmed_ = false;
            needs_redraw_ = true;
        }
        if (at_end_ && w_ < total_weight_ - 1e-6) at_end_ = false;
    }

    if (message_visible() || rec_phase_ != RecPhase::Idle) needs_redraw_ = true;
    if (!options_.capture_path.empty() && frames_drawn_ < 6) needs_redraw_ = true;

    if (total_weight_ > 0.0) {
        scroll_ = std::max(0.0, std::min(scroll_,
            static_cast<double>(layout_.max_scroll)));
    }
}

void App::onHostResized() {
    if (renderer_) renderer_->notifyResized();
    layout_dirty_ = true;
    library_reveal_ = true;
    needs_redraw_ = true;
}

void App::onHostLayoutInvalidated() {
    layout_dirty_ = true;
    needs_redraw_ = true;
}

void App::onHostExposed() {
    needs_redraw_ = true;
}

void App::onTimer(int timerId) {
    if (!host_) return;
    if (timerId == 1) tick();
}

void App::run() {
    while (!quit_) {
        host_->pump(false);
        if (host_->quitRequested()) {
            shutdown();
            break;
        }
        if (needs_redraw_) {
            draw_frame();
            needs_redraw_ = false;
        }
    }
    shutdown();
    // The host may have called shutdown() from inside its own pump(); it is
    // only safe to destroy it after pump() has returned.
    host_.reset();
}

void App::shutdown() {
    if (shutdown_done_) return;
    shutdown_done_ = true;
    quit_ = true;
    if (host_) host_->stopTimer(1);
    if (recorder_) recorder_->stop_blocking();
    renderer_.reset();
}

namespace {

bool is_toggle_key(int key) {
    switch (key) {
    case key::Space: case 0x52: case 0x46: case 0x48: case 0x4C:
    case 0x43: case 0x41: case 0x4B: case 0x4D: case key::F1:
    case 0x74: case 0x7A: case 0x3F: case 0x54:
        return true;
    default:
        return false;
    }
}

} // namespace

void App::onKeyDownPortable(int keyCode) {
    if (!host_) return;
    if (keyCode == key::Control) { ctrl_down_ = true; return; }
    if (keyCode == key::Shift) return;

    const double now = now_monotonic();
    const int slot = keyCode & 0xFF;
    const bool repeated = now - last_repeat_[slot] < 0.35;
    last_repeat_[slot] = now;

    if (ctrl_down_ && (keyCode == 0x51)) { // Ctrl+Q
        quit_ = true;
        return;
    }
    if (help_visible_) {
        help_visible_ = false;
        needs_redraw_ = true;
        return;
    }
    if (font_menu_open_) {
        const auto& presets = gui::font_presets();
        switch (keyCode) {
        case key::Escape:
            font_menu_open_ = false;
            break;
        case 0x54: // T
            if (!repeated) font_menu_open_ = false;
            break;
        case key::Up:
            font_menu_selected_ = std::max(0, font_menu_selected_ - 1);
            break;
        case key::Down:
            font_menu_selected_ = std::min(static_cast<int>(presets.size()) - 1,
                                           font_menu_selected_ + 1);
            break;
        case key::Home:
            font_menu_selected_ = 0;
            break;
        case key::End:
            font_menu_selected_ = static_cast<int>(presets.size()) - 1;
            break;
        case key::Enter: {
            const int index = std::max(0, std::min(font_menu_selected_,
                                                  static_cast<int>(presets.size()) - 1));
            const gui::FontPreset& preset = presets[static_cast<std::size_t>(index)];
            font_menu_open_ = false;
            apply_font(preset, true);
            break;
        }
        default:
            break;
        }
        needs_redraw_ = true;
        return;
    }
    if (ctrl_down_ && (keyCode == 0x52)) { // Ctrl+R
        if (repeated) return;
        if (is_prompter()) {
            go_home();
        } else {
            recents_ = load_recents();
            refresh_library();
            show_message("Recent scripts refreshed");
        }
        return;
    }
    if (ctrl_down_ && (keyCode == 0x4F)) { // Ctrl+O
        if (repeated) return;
        open_picker();
        return;
    }

    if (!is_prompter()) {
        if (picker_open_) {
            switch (keyCode) {
            case key::Escape:
                picker_open_ = false;
                break;
            case key::Up:
                picker_selected_ = std::max(0, picker_selected_ - 1);
                break;
            case key::Down:
                picker_selected_ = std::min(static_cast<int>(picker_entries_.size()) - 1,
                                            picker_selected_ + 1);
                break;
            case key::Home:
                picker_selected_ = 0;
                break;
            case key::End:
                picker_selected_ = std::max(0, static_cast<int>(picker_entries_.size()) - 1);
                break;
            case key::PageUp:
                picker_selected_ = std::max(0, picker_selected_ - 10);
                break;
            case key::PageDown:
                picker_selected_ = std::min(static_cast<int>(picker_entries_.size()) - 1,
                                            picker_selected_ + 10);
                break;
            case key::Enter:
                picker_activate(picker_selected_);
                break;
            case key::Backspace:
                picker_up();
                break;
            default:
                break;
            }
            needs_redraw_ = true;
            return;
        }
        if (keyCode == 0x54) { // T
            if (repeated) return;
            open_font_menu();
            return;
        }
        if (library_entries_.empty()) return;
        if (keyCode == key::Up) {
            library_selected_ = std::max(0, library_selected_ - 1);
            library_reveal_ = true;
        } else if (keyCode == key::Down) {
            library_selected_ = std::min(static_cast<int>(library_entries_.size()) - 1,
                                         library_selected_ + 1);
            library_reveal_ = true;
        } else if (keyCode == key::Home) {
            library_selected_ = 0;
            library_reveal_ = true;
        } else if (keyCode == key::End) {
            library_selected_ = static_cast<int>(library_entries_.size()) - 1;
            library_reveal_ = true;
        } else if (keyCode == key::Enter || keyCode == key::Space) {
            const LibraryEntry& entry = library_entries_[static_cast<std::size_t>(library_selected_)];
            if (entry.sample) open_test_script();
            else if (entry.valid) open_script(entry.path);
            else show_message(entry.title + ": timing is incomplete", true);
        } else {
            return;
        }
        needs_redraw_ = true;
        return;
    }

    if (is_toggle_key(keyCode) && repeated) return;

    switch (keyCode) {
    case key::Space: toggle_play(); break;
    case 0x52: toggle_recording(); break; // R
    case 0x46: case 0x7A: toggle_fullscreen(); break; // F
    case key::Escape:
        if (fullscreen_) {
            fullscreen_ = false;
            host_->setFullscreen(false);
            needs_redraw_ = true;
        }
        break;
    case 0x3F: case key::F1: help_visible_ = true; needs_redraw_ = true; break;
    case 0x48: // H
        hud_visible_ = !hud_visible_;
        if (!hud_visible_) hud_dimmed_ = false;
        needs_redraw_ = true;
        break;
    case 0x54: open_font_menu(); break; // T
    case 0x43: cycle_cue(); break; // C
    case 0x41: centered_ = !centered_; layout_dirty_ = true;
        show_message(centered_ ? "Text: centred" : "Text: left-aligned"); break;
    case 0x4B: karaoke_ = !karaoke_; needs_redraw_ = true;
        show_message(karaoke_ ? "Word karaoke on" : "Word karaoke off"); break;
    case 0x4D: mood_on_ = !mood_on_; needs_redraw_ = true;
        show_message(mood_on_ ? "Mood colours on" : "Mood colours off"); break;
    case 0x4C: case 0x74: reload_script(true); break;
    case key::Up: cue_fraction_ = clampf(cue_fraction_ - kCueStep, kCueMin, kCueMax);
        needs_redraw_ = true; break;
    case key::Down: cue_fraction_ = clampf(cue_fraction_ + kCueStep, kCueMin, kCueMax);
        needs_redraw_ = true; break;
    case key::Left: nudge(-std::max(60.0, renderer_->height() * 0.15)); break;
    case key::Right: nudge(std::max(60.0, renderer_->height() * 0.15)); break;
    case key::Home: seek(0.0); break;
    case key::End: seek(layout_.max_scroll); break;
    default: break;
    }
}

void App::onKeyUpPortable(int keyCode) {
    if (keyCode == key::Control) ctrl_down_ = false;
}

void App::onMouseMove(int x, int y) {
    // The Wayland backend can dispatch a final pointer event while the host is
    // being torn down, after App::run() has already reset host_.
    if (!host_) return;
    last_motion_t_ = now_monotonic();
    if (hud_dimmed_) {
        hud_dimmed_ = false;
        needs_redraw_ = true;
    }
    const Hit* hit = hit_at(static_cast<float>(x), static_cast<float>(y));
    const int want = hit != nullptr ? 1 : 0;
    if (want != cursor_) {
        cursor_ = want;
        host_->setCursor(want ? CursorShape::Hand : CursorShape::Arrow);
    }
}

void App::onMouseLeave() {
    if (!host_) return;
    if (cursor_ != 0) {
        cursor_ = 0;
        host_->setCursor(CursorShape::Arrow);
    }
}

void App::onLButtonDown(int x, int y) {
    if (!host_) return;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (help_visible_) {
        help_visible_ = false;
        needs_redraw_ = true;
        return;
    }
    if (font_menu_open_) {
        for (const Hit& hit : hits_) {
            if (hit.id.rfind("font:", 0) == 0 && hit.contains(fx, fy)) {
                const int index = std::atoi(hit.id.c_str() + 5);
                const auto& presets = gui::font_presets();
                if (index >= 0 && index < static_cast<int>(presets.size())) {
                    font_menu_open_ = false;
                    apply_font(presets[static_cast<std::size_t>(index)], true);
                }
                return;
            }
        }
        font_menu_open_ = false;
        needs_redraw_ = true;
        return;
    }
    if (picker_open_) {
        for (const Hit& hit : hits_) {
            if (hit.id.rfind("pick:", 0) == 0 && hit.contains(fx, fy)) {
                picker_activate(std::atoi(hit.id.c_str() + 5));
                return;
            }
        }
        if (!hits_.empty()) {
            bool inside = false;
            for (const Hit& hit : hits_) {
                if (hit.id == "picker-panel" && hit.contains(fx, fy)) inside = true;
            }
            if (!inside) {
                picker_open_ = false;
                needs_redraw_ = true;
            }
        }
        return;
    }
    const Hit* hit = hit_at(fx, fy);
    if (hit == nullptr) return;
    if (hit->id == "open") {
        open_picker();
    } else if (hit->id == "font") {
        open_font_menu();
    } else if (hit->id == "home") {
        go_home();
    } else if (hit->id == "record") {
        toggle_recording();
    } else if (hit->id == "cue") {
        cycle_cue();
    } else if (hit->id.rfind("card:", 0) == 0) {
        const int index = std::atoi(hit->id.c_str() + 5);
        if (index >= 0 && index < static_cast<int>(library_entries_.size())) {
            library_selected_ = index;
            const LibraryEntry& entry = library_entries_[static_cast<std::size_t>(index)];
            if (entry.sample) open_test_script();
            else if (entry.valid) open_script(entry.path);
            else show_message(entry.title + ": timing is incomplete", true);
        }
    }
    needs_redraw_ = true;
}

void App::onLButtonUp(int, int) {}

void App::onMouseWheel(int x, int y, int delta) {
    if (!host_) return;
    (void)x;
    (void)y;
    if (font_menu_open_) return;
    if (picker_open_) {
        picker_selected_ = std::max(0, std::min(static_cast<int>(picker_entries_.size()) - 1,
                                             picker_selected_ - (delta > 0 ? 1 : -1)));
        needs_redraw_ = true;
        return;
    }
    if (!is_prompter()) {
        library_scroll_ = std::max(0.0f, library_scroll_ - delta * 0.6f);
        library_reveal_ = false;
        needs_redraw_ = true;
        return;
    }
    nudge(-delta * 0.6);
}

void App::collect_all_text() {
    static const char* fixed[] = {
        "Teleprompter", "Pick up a recent script, or open one from disk.",
        "Open script", "RECENT SCRIPTS", "ALWAYS AVAILABLE", "Test script",
        "Built-in demo score · read-only", "Always available", "needs timing",
        "No recent scripts yet", "Open a script and it will appear here next time.",
        "Ctrl+O open  ·  Ctrl+R refresh  ·  Ctrl+Q quit", "Scripts", "Record", "Stop",
        "Finalizing…", "Ready", "Playing", "Paused", "Done", "No script",
        "left", "left of", "Recording", "Saved", "Reading line", "Text: centred",
        "Text: left-aligned", "Word karaoke on", "Word karaoke off",
        "Mood colours on", "Mood colours off", "Recent scripts refreshed",
        "No scripts here", "Enter opens · Backspace up · Esc closes",
        "Keyboard shortcuts", "Esc or ? closes this card", "SAMPLE", "Hold",
        "Script font", "Font", "Enter applies · Esc closes", "Current",
    };
    bool added = false;
    auto collect = [&](std::string_view value) {
        added = text_.collect(value) || added;
    };
    for (const char* text : fixed) collect(text);
    for (const auto& [keys, description] : key_help()) {
        collect(keys);
        collect(description);
    }
    for (const Paragraph& paragraph : plan_) {
        collect(paragraph.text);
        collect(paragraph.theme);
        for (const Word& word : paragraph.words) {
            collect(word.text);
            collect(word.theme);
        }
    }
    for (const LibraryEntry& entry : library_entries_) {
        collect(entry.title);
        collect(entry.subtitle);
        collect(entry.meta);
    }
    for (const PickerEntry& entry : picker_entries_) collect(entry.name);
    for (const gui::FontPreset& preset : gui::font_presets()) collect(preset.label);
    collect(current_font_.label);
    collect(picker_dir_);
    collect(message_);
    collect(theme_text());
    if (recorder_) collect(fs::path(recorder_->path()).filename().string());
    font_dirty_ = font_dirty_ || added;
    text_dirty_ = false;
}

void App::refresh_text_font() {
    if (text_dirty_) collect_all_text();
    if (font_dirty_ || !msdf_initialized_) sync_font();
}

void App::sync_font() {
    if (!renderer_ || !text_.valid()) return;
    if (text_.has_misses()) {
        if (text_.bake_misses() > 0) renderer_->initMsdf(*text_.font());
    }
    const float effective_script = script_px_ * current_font_.size_scale;
    const int direction = static_cast<int>(std::lround(effective_script * 0.68f));
    const int added = text_.ensure({
        static_cast<int>(std::lround(effective_script)), direction,
        10, 11, 12, 13, 14, 15, 16, 18, 19, 22, 26, 30});
    if (added > 0 || !msdf_initialized_) {
        renderer_->initMsdf(*text_.font());
        msdf_initialized_ = true;
    }
    font_dirty_ = false;
}

void App::put_text(std::string_view text, float x, float top, float size, Rgb colour,
                   FontStyle style, float alpha, bool centered) {
    put_text_clipped(text, x, top, size, colour, style, alpha, centered, ClipRect{});
}

void App::put_text_clipped(std::string_view text, float x, float top, float size, Rgb colour,
                           FontStyle style, float alpha, bool centered,
                           const ClipRect& clip) {
    if (text.empty()) return;
    if (centered) x -= text_.width(text, size, style) * 0.5f;
    const std::vector<Glyph> glyphs = text_.layout(text, x, top + size, size, style);
    text_.append(text_quads_, glyphs, to_color(colour, alpha), clip);
}

void App::add_hit(std::string id, float x, float y, float w, float h) {
    hits_.push_back(Hit{std::move(id), x, y, w, h});
}

const App::Hit* App::hit_at(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->contains(x, y)) return &*it;
    }
    return nullptr;
}

void App::draw_frame() {
    if (!renderer_) return;
    const uint32_t width = renderer_->width();
    const uint32_t height = renderer_->height();
    if (width == 0 || height == 0) return;

    refresh_text_font();

    if (is_prompter() && (layout_dirty_ || width != layout_width_ || height != layout_height_)) {
        rebuild_layout();
        rebuild_timeline();
        layout_dirty_ = false;
    }

    shapes_.clear();
    text_quads_.clear();
    hits_.clear();

    Canvas canvas(shapes_, width, height, nullptr, 0, 0, 0, 0);
    canvas.useShapes(&shapes_);

    if (help_visible_) {
        draw_help(canvas);
    } else if (font_menu_open_) {
        draw_font_menu(canvas);
    } else if (picker_open_) {
        draw_picker(canvas);
    } else if (is_prompter()) {
        draw_prompter(canvas);
    } else {
        draw_library(canvas);
    }

    renderer_->draw({}, 0, {}, {}, text_quads_, shapes_);
    ++frames_drawn_;

    if (!options_.capture_path.empty() && frames_drawn_ >= 6) {
        std::vector<uint8_t> rgba;
        uint32_t out_w = 0, out_h = 0;
        if (renderer_->readbackLastFrame(rgba, out_w, out_h)) {
            std::ofstream out(options_.capture_path, std::ios::binary | std::ios::trunc);
            out << "P6\n" << out_w << " " << out_h << "\n255\n";
            for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
                out.put(static_cast<char>(rgba[i]));
                out.put(static_cast<char>(rgba[i + 1]));
                out.put(static_cast<char>(rgba[i + 2]));
            }
            std::fprintf(stderr, "captured %s (%ux%u)\n", options_.capture_path.c_str(),
                         out_w, out_h);
        } else {
            std::fprintf(stderr, "capture failed: readback unavailable\n");
        }
        quit_ = true;
    }

    if (renderer_->width() != width || renderer_->height() != height) {
        layout_dirty_ = true;
        needs_redraw_ = true;
    }
}

void App::draw_library(Canvas& canvas) {
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    canvas.clear(rgb(kLibraryBg));

    put_text("Teleprompter", 38, 28, 30, kText, FontStyle::Bold);
    put_text("Pick up a recent script, or open one from disk.", 38, 70, 14, kDim);

    const float open_w = 190.0f;
    const float open_x = width - 38.0f - open_w;
    const float font_w = std::min(230.0f, std::max(150.0f, open_x - 38.0f - 180.0f));
    const float font_x = open_x - 14.0f - font_w;
    canvas.rect(font_x, 32, font_w, 42, rgb(kPanel), kRadiusControl);
    canvas.rectOutline(font_x, 32, font_w, 42, rgb(kBorder), kRadiusControl);
    add_hit("font", font_x, 32, font_w, 42);
    put_text(preview_text("Font: " + current_font_.label, 26), font_x + 14, 43, 13,
             kSoft);
    canvas.rect(open_x, 32, open_w, 42, rgb(kAmber), kRadiusControl);
    add_hit("open", open_x, 32, open_w, 42);
    put_text("Open script", open_x + open_w * 0.5f, 43, 15, kButtonInk,
             FontStyle::Bold, 1.0f, true);

    if (message_visible()) {
        const Rgb colour = message_error_ ? kErrorText : kSoft;
        put_text(message_, 38, 96, 13, colour);
    }

    const float card_gap = 14.0f;
    const int columns = 3;
    const float card_w = std::max(240.0f, (width - 2 * 38.0f - (columns - 1) * card_gap) / columns);
    const float card_h = 112.0f;
    const float row_h = card_h + card_gap;
    const int recent_count = std::max(0, static_cast<int>(library_entries_.size()) - 1);
    const int rows = (recent_count + columns - 1) / columns;
    const float sample_offset =
        (rows > 0 ? static_cast<float>(rows) * row_h : 52.0f) + 16.0f + 26.0f;
    const float content_bottom = 158.0f + sample_offset + card_h;
    const float max_scroll = std::max(0.0f, content_bottom - (height - 24.0f));
    library_scroll_ = clampf(library_scroll_, 0.0f, max_scroll);

    if (library_reveal_) {
        float selected_offset = -1.0f;
        if (library_selected_ == static_cast<int>(library_entries_.size()) - 1) {
            selected_offset = sample_offset;
        } else if (library_selected_ >= 0 && library_selected_ < recent_count) {
            const int row = library_selected_ / columns;
            selected_offset = static_cast<float>(row) * row_h;
        }
        if (selected_offset >= 0.0f) {
            const float top = 158.0f;
            const float bottom = height - 24.0f;
            const float y = 158.0f - library_scroll_ + selected_offset;
            if (y < top) {
                library_scroll_ = std::max(0.0f, library_scroll_ - (top - y));
            } else if (y + card_h > bottom) {
                library_scroll_ += y + card_h - bottom;
            }
            library_scroll_ = clampf(library_scroll_, 0.0f, max_scroll);
        }
        library_reveal_ = false;
    }

    put_text("RECENT SCRIPTS", 38, 132, 12, kDim, FontStyle::Bold);

    const float start_y = 158.0f - library_scroll_;
    if (recent_count == 0) {
        put_text("No recent scripts yet", 38, start_y + 4, 16, kSoft, FontStyle::Bold);
        put_text("Open a script and it will appear here next time.", 38, start_y + 30, 13, kDim);
    } else {
        for (int i = 0; i < recent_count; ++i) {
            const int col = i % columns;
            const int row = i / columns;
            const float x = 38.0f + static_cast<float>(col) * (card_w + card_gap);
            const float y = start_y + static_cast<float>(row) * row_h;
            if (y + card_h < 130 || y > height - 24) continue;
            const LibraryEntry& entry = library_entries_[static_cast<std::size_t>(i)];
            canvas.rect(x, y, card_w, card_h, rgb(kPanel), kRadiusControl);
            const Rgb border = i == library_selected_ ? kAmber : kCardBorder;
            canvas.rectOutline(x, y, card_w, card_h, rgb(border), kRadiusControl);
            add_hit("card:" + std::to_string(i), x, y, card_w, card_h);
            put_text(preview_text(entry.title, 34), x + 16, y + 14, 16, kText, FontStyle::Bold);
            put_text(preview_text(entry.subtitle, 44), x + 16, y + 44, 12, kDim);
            put_text(entry.meta, x + 16, y + 74, 12,
                     entry.valid ? kStateText : kAmber);
        }
    }

    const float sample_heading = start_y + sample_offset - 26.0f;
    put_text("ALWAYS AVAILABLE", 38, sample_heading, 12, kDim, FontStyle::Bold);
    const float sample_y = start_y + sample_offset;
    const LibraryEntry& sample = library_entries_.back();
    if (sample_y + card_h > 120 && sample_y < height - 24) {
        const bool selected = library_selected_ == static_cast<int>(library_entries_.size()) - 1;
        canvas.rect(38, sample_y, card_w, card_h, rgb(kSamplePanel), kRadiusControl);
        const Rgb border = selected ? kAmber : kSampleBorder;
        canvas.rectOutline(38, sample_y, card_w, card_h, rgb(border), kRadiusControl);
        add_hit("card:" + std::to_string(library_entries_.size() - 1),
                38, sample_y, card_w, card_h);
        put_text("Test script", 54, sample_y + 14, 16, kText, FontStyle::Bold);
        put_text(sample.subtitle, 54, sample_y + 44, 12, kDim);
        put_text(sample.meta, 54, sample_y + 74, 12, kAmber);
        const float badge_w = 62.0f;
        canvas.rect(38 + card_w - badge_w - 14, sample_y + 14, badge_w, 20,
                    rgb(kSampleBadge), 10);
        put_text("SAMPLE", 38 + card_w - badge_w * 0.5f - 14, sample_y + 18, 10,
                 kAmber, FontStyle::Bold, 1.0f, true);
    }

    put_text("Ctrl+O open  ·  Ctrl+R refresh  ·  Ctrl+Q quit", 38, height - 28, 12, kFaint);
}

void App::draw_picker(Canvas& canvas) {
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    canvas.clear(rgb(kModalBg));
    canvas.rect(0, 0, width, height, ::Color{0, 0, 0, 0.55f});

    const float panel_w = std::min(760.0f, width - 80.0f);
    const float panel_h = std::min(580.0f, height - 90.0f);
    const float x = (width - panel_w) * 0.5f;
    const float y = (height - panel_h) * 0.5f;
    canvas.rect(x, y, panel_w, panel_h, rgb(kModalPanel), kRadiusPanel);
    canvas.rectOutline(x, y, panel_w, panel_h, rgb(kBorder), kRadiusPanel);
    add_hit("picker-panel", x, y, panel_w, panel_h);

    put_text("Open a script", x + 24, y + 18, 19, kText, FontStyle::Bold);
    put_text(preview_text(picker_dir_, 82), x + 24, y + 46, 12, kDim);

    const int visible = std::max(1, static_cast<int>((panel_h - 112) / kPickRow));
    if (picker_selected_ < picker_scroll_) picker_scroll_ = picker_selected_;
    if (picker_selected_ >= picker_scroll_ + visible) {
        picker_scroll_ = picker_selected_ - visible + 1;
    }
    const float rows_y = y + 74.0f;
    if (picker_entries_.empty()) {
        put_text("No scripts here", x + 24, rows_y + 8, 14, kDim);
    }
    for (int i = picker_scroll_;
         i < static_cast<int>(picker_entries_.size()) && i < picker_scroll_ + visible; ++i) {
        const PickerEntry& entry = picker_entries_[static_cast<std::size_t>(i)];
        const float row_y =
            rows_y + static_cast<float>(i - picker_scroll_) * kPickRow;
        if (i == picker_selected_) {
            canvas.rect(x + 12, row_y, panel_w - 24, kPickRow - 2,
                        rgb(kSelectedBg), kRadiusSmall);
        }
        put_text(entry.name, x + 24, row_y + 7, 14,
                 entry.directory ? kAmber : kText,
                 entry.directory ? FontStyle::Bold : FontStyle::Roman);
        add_hit("pick:" + std::to_string(i), x + 12, row_y, panel_w - 24, kPickRow - 2);
    }
    put_text("Enter opens · Backspace up · Esc closes", x + 24, y + panel_h - 28, 12, kFaint);
}

void App::draw_prompter(Canvas& canvas) {
    canvas.clear(rgb(kBg));
    draw_script(canvas);
    draw_cue_bars(canvas);
    draw_progress(canvas);
    draw_section(canvas);
    draw_hud(canvas);
    draw_rec_frame(canvas);
}

void App::draw_script(Canvas& canvas) {
    if (layout_.glyphs.empty()) return;
    const float cue_y = static_cast<float>(canvas.h()) * cue_fraction_;
    const float dy = cue_y - static_cast<float>(scroll_);
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    float top_clip = 0.0f;
    if (hud_visible_) {
        top_clip = 80.0f;
        if (message_visible()) top_clip = 100.0f;
        if (!at_end_ && total_weight_ > 0.0) {
            const std::optional<SectionState> section =
                section_state(plan_, w_, &paragraph_starts_);
            if (section && section->remaining > 1e-9) top_clip = 146.0f;
        }
    }
    canvas.setClip(0, top_clip, width, height - top_clip);

    for (const gui::PlacedParagraph& paragraph : layout_.paragraphs) {
        if (!paragraph.direction || paragraph.card_w <= 0) continue;
        const float y = paragraph.card_y + dy;
        if (y > height || y + paragraph.card_h < 0 || y < top_clip) continue;
        canvas.rect(paragraph.card_x, y, paragraph.card_w, paragraph.card_h,
                    ::Color{0.14f, 0.11f, 0.06f, 0.92f}, kRadiusControl);
        canvas.rectOutline(paragraph.card_x, y, paragraph.card_w, paragraph.card_h,
                           ::Color{0.94f, 0.63f, 0.24f, 0.28f}, kRadiusControl);
        const float bar_inset = std::min(kRadiusControl, paragraph.card_h * 0.5f);
        canvas.rect(paragraph.card_x + 1.0f, y + bar_inset, 4.0f,
                    std::max(0.0f, paragraph.card_h - 2.0f * bar_inset),
                    ::Color{0.94f, 0.63f, 0.24f, 0.70f}, 2);
    }

    for (const gui::PlacedWord& word : layout_.words) {
        if (!word.comment || word.direction) continue;
        for (const gui::PlacedLine& line : word.lines) {
            const float y = line.center + dy;
            const float h = effective_script_px() * 1.24f;
            const float x = line.x0 - 4.0f;
            const float w = std::max(2.0f, line.x1 - line.x0 + 8.0f);
            if (y + h < 0 || y - h > height || y - h * 0.5f < top_clip) continue;
            canvas.rect(x, y - h * 0.5f, w, h, ::Color{0.16f, 0.13f, 0.08f, 0.96f}, 5);
        }
    }

    script_frame_.clear();
    script_colours_.clear();
    script_frame_.reserve(layout_.glyphs.size());
    script_colours_.reserve(layout_.glyphs.size());

    const ::Color text_colour = to_color(kText);
    const ::Color gray = to_color(kKaraokeGrayRgb);
    int last_word = -1;
    bool word_comment = false;
    double word_progress = 0.0;
    Rgb word_lit = kText;
    Rgb comment_rgb = kAmberComment;
    int word_chars = 0;

    for (const Glyph& source : layout_.glyphs) {
        if (!source.draw) continue;
        Glyph glyph = source;
        glyph.y0 += dy;
        glyph.y1 += dy;
        glyph.line_center += dy;
        // Drop a whole glyph rather than clipping it at the overlay band: half
        // a letter peeking out from under the HUD reads as a rendering bug.
        if (glyph.line_center < top_clip) continue;
        if (glyph.y0 >= height) continue;

        const int word_index = glyph.word_index;
        ::Color colour = text_colour;
        if (word_index >= 0 &&
            static_cast<std::size_t>(word_index) < flat_words_.size()) {
            if (word_index != last_word) {
                last_word = word_index;
                const Word& word = flat_words_[static_cast<std::size_t>(word_index)];
                word_comment = word.is_comment;
                word_progress = word_fill(word.clock, word.seconds, w_);
                if (word_comment) {
                    comment_rgb = karaoke_ ? comment_colour(word_progress) : kAmberComment;
                } else {
                    word_lit = mood_on_ ? mood_colour(word.mood) : kText;
                    word_chars = (karaoke_ && word_progress > 0.0 && word_progress < 1.0)
                        ? gui::utf8_codepoint_count(word.text) : 0;
                }
            }
            if (word_comment) {
                colour = to_color(comment_rgb);
            } else if (!karaoke_ || word_progress >= 1.0) {
                colour = to_color(word_lit);
            } else if (word_progress <= 0.0) {
                colour = gray;
            } else {
                const double cover =
                    character_fill(word_progress, glyph.char_index, word_chars);
                colour = to_color(mix(kKaraokeGrayRgb, word_lit, cover));
            }
        }
        script_frame_.push_back(glyph);
        script_colours_.push_back(colour);
    }
    canvas.clearClip();
    ClipRect clip{true, 0, top_clip, width, height};
    text_.append(text_quads_, script_frame_, script_colours_, clip);
}

void App::draw_cue_bars(Canvas& canvas) {
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    const float cue_y = height * cue_fraction_;
    const float side = std::max(effective_script_px(), (width - layout_.column_width) * 0.5f);
    const float bar_w = std::max(12.0f, side - 8.0f);
    const float bar_h = std::max(3.0f, effective_script_px() * 0.09f);
    const float y = cue_y - bar_h * 0.5f;
    canvas.rect(4, y, bar_w, bar_h, ::Color{0.94f, 0.63f, 0.24f, 0.90f}, 3);
    canvas.rect(width - 4 - bar_w, y, bar_w, bar_h, ::Color{0.94f, 0.63f, 0.24f, 0.90f}, 3);
}

void App::draw_progress(Canvas& canvas) {
    if (total_weight_ <= 0.0) return;
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    const float fraction = static_cast<float>(std::max(0.0, std::min(1.0, w_ / total_weight_)));
    canvas.rect(0, height - 3, width * fraction, 3, ::Color{0.94f, 0.63f, 0.24f, 0.90f});
}

void App::draw_rec_frame(Canvas& canvas) {
    if (rec_phase_ == RecPhase::Idle) return;
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    const ::Color red{1.0f, 0.37f, 0.34f, 0.85f};
    canvas.rect(0, 0, width, 2, red);
    canvas.rect(0, height - 2, width, 2, red);
    canvas.rect(0, 0, 2, height, red);
    canvas.rect(width - 2, 0, 2, height, red);
}

void App::draw_hud(Canvas& canvas) {
    if (!hud_visible_) return;
    const float width = static_cast<float>(canvas.w());
    const float alpha = hud_dimmed_ ? 0.30f : 1.0f;
    const ::Color panel{0.07f, 0.07f, 0.07f, 0.62f * alpha};

    canvas.rect(kHudMargin, kHudMargin, width - 2 * kHudMargin, kHudHeight, panel,
                kRadiusControl);

    // Scripts
    canvas.rect(18, 18, 96, 34, ::Color{1, 1, 1, 0.06f * alpha}, kRadiusSmall);
    add_hit("home", 18, 18, 96, 34);
    put_text("Scripts", 66, 27, 14, kText, FontStyle::Roman, alpha, true);

    // Record
    const bool recording = rec_phase_ == RecPhase::Recording;
    const bool finalizing = rec_phase_ == RecPhase::Finalizing;
    canvas.rect(120, 18, 118, 34,
                recording ? ::Color{0.55f, 0.08f, 0.10f, 0.55f * alpha}
                          : ::Color{1, 1, 1, 0.06f * alpha}, kRadiusSmall);
    add_hit("record", 120, 18, 118, 34);
    const std::string record_label = recording ? "Stop" : (finalizing ? "Finalizing…" : "Record");
    put_text(record_label, 179, 27, 14, recording ? kRed : kText,
             recording ? FontStyle::Bold : FontStyle::Roman, alpha, true);

    // State chip
    const std::string state = state_text();
    const float state_w = text_.width(state, 13, FontStyle::Bold) + 18.0f;
    canvas.rect(250, 22, state_w, 26, ::Color{1, 1, 1, 0.07f * alpha}, 13);
    put_text(state, 250 + state_w * 0.5f, 27, 13, kStateText,
             FontStyle::Bold, alpha, true);

    float cursor_x = 250 + state_w + 14.0f;
    if (recording) {
        const double blink = std::fmod(now_monotonic(), 1.2);
        const float dot_alpha = (blink < 0.6 ? 1.0f : 0.2f) * alpha;
        canvas.rect(cursor_x, 31, 10, 10, ::Color{1.0f, 0.37f, 0.34f, dot_alpha}, 5);
        cursor_x += 18;
        const std::string timer = "REC " + format_time(now_monotonic() - rec_started_);
        put_text(timer, cursor_x, 28, 13, kRed, FontStyle::Bold, alpha);
        cursor_x += text_.width(timer, 13, FontStyle::Bold) + 16;
    }
    const std::string theme = theme_text();
    if (!theme.empty()) {
        put_text(theme, cursor_x, 28, 13, kThemeText, FontStyle::Roman, alpha);
    }

    // Right cluster.
    const std::string total = format_time(duration_);
    const float total_w = text_.width(total, 16, FontStyle::Bold);
    put_text(total, width - 24 - total_w, 25, 16, kText, FontStyle::Bold, alpha);

    const std::string cue = "- " + std::to_string(static_cast<int>(std::lround(cue_fraction_ * 100))) + "%";
    const float cue_w = 78.0f;
    const float cue_x = width - 24 - total_w - 18 - cue_w;
    canvas.rect(cue_x, 21, cue_w, 27, ::Color{1, 1, 1, 0.06f * alpha},
                kRadiusSmall);
    add_hit("cue", cue_x, 21, cue_w, 27);
    put_text(cue, cue_x + cue_w * 0.5f, 27, 13, kSoft, FontStyle::Roman, alpha, true);

    const std::string remaining = remaining_text();
    if (!remaining.empty()) {
        const double seconds_left = std::max(0.0, (1.0 - w_ / total_weight_) * duration_);
        const Rgb colour = seconds_left <= 10.0 ? kRed
            : (seconds_left <= 30.0 ? kAmber : kSoft);
        put_text(remaining, cue_x - 16 - text_.width(remaining, 14, FontStyle::Roman),
                 27, 14, colour, FontStyle::Roman, alpha);
    }

    if (message_visible()) {
        const float msg_w = std::min(width - 40.0f,
            text_.width(message_, 13, FontStyle::Roman) + 24.0f);
        canvas.rect(18, 66, msg_w, 28,
                    ::Color{0.06f, 0.06f, 0.06f, 0.82f * alpha}, kRadiusSmall);
        put_text(message_, 30, 72, 13,
                 message_error_ ? kErrorText : kSoft,
                 FontStyle::Roman, alpha);
    }
}

void App::draw_section(Canvas& canvas) {
    if (!hud_visible_ || at_end_ || total_weight_ <= 0.0 || plan_.empty()) return;
    const std::optional<SectionState> state =
        section_state(plan_, w_, &paragraph_starts_);
    if (!state || state->remaining <= 1e-9) return;

    const float width = static_cast<float>(canvas.w());
    const float card_w = std::min(560.0f, width - 40.0f);
    const float card_h = 64.0f;
    const float x = (width - card_w) * 0.5f;
    const float y = 70.0f;
    canvas.rect(x, y, card_w, card_h, ::Color{0.08f, 0.06f, 0.03f, 0.95f},
                kRadiusControl);
    canvas.rectOutline(x, y, card_w, card_h,
                       ::Color{0.94f, 0.63f, 0.24f, 0.38f}, kRadiusControl);

    const float countdown_size = 13.0f;
    const std::string countdown = format_time(state->remaining) + " left of " +
                                  format_time(state->paragraph->seconds);
    const float countdown_w = text_.width(countdown, countdown_size, FontStyle::Bold);
    const float title_max = std::max(24.0f, card_w - 28.0f - countdown_w - 14.0f);
    const std::string title = fit_text(
        text_, section_title(*state->paragraph, std::numeric_limits<int>::max()),
        title_max, 12.0f, FontStyle::Bold);
    put_text(title, x + 14, y + 11, 12, kThemeText, FontStyle::Bold);
    put_text(countdown, x + card_w - 14 - countdown_w, y + 10, countdown_size,
             kAmberComment, FontStyle::Bold);

    canvas.rect(x + 14, y + 46, card_w - 28, 4, ::Color{1, 1, 1, 0.12f}, 2);
    canvas.rect(x + 14, y + 46, (card_w - 28) * static_cast<float>(state->progress), 4,
                ::Color{0.94f, 0.63f, 0.24f, 1.0f}, 2);
}

void App::draw_font_menu(Canvas& canvas) {
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    canvas.clear(rgb(kModalBg));
    canvas.rect(0, 0, width, height, ::Color{0, 0, 0, 0.74f});

    const auto& presets = gui::font_presets();
    const float panel_w = std::min(660.0f, width - 60.0f);
    const float panel_h = std::min(360.0f, height - 60.0f);
    const float x = (width - panel_w) * 0.5f;
    const float y = (height - panel_h) * 0.5f;
    canvas.rect(x, y, panel_w, panel_h, rgb(kModalPanel), kRadiusPanel);
    canvas.rectOutline(x, y, panel_w, panel_h, rgb(kBorder), kRadiusPanel);

    put_text("Script font", x + 28, y + 20, 19, kText, FontStyle::Bold);
    put_text("Choose the face used across the app.",
             x + 28, y + 50, 13, kDim);

    const float row_h = 52.0f;
    const float first = y + 84.0f;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const float row_y = first + static_cast<float>(i) * row_h;
        const bool selected = static_cast<int>(i) == font_menu_selected_;
        const bool current = presets[i].id == current_font_.id;
        if (selected) {
            canvas.rect(x + 16, row_y, panel_w - 32, row_h - 6,
                        rgb(kSelectedBg), kRadiusSmall);
        }
        put_text(presets[i].label, x + 32, row_y + 9, 16,
                 selected ? kAmberLit : kText, FontStyle::Bold);
        if (current) {
            put_text("Current", x + panel_w - 92, row_y + 12, 12, kAmber);
        }
        add_hit("font:" + std::to_string(i), x + 16, row_y, panel_w - 32, row_h - 6);
    }

    put_text("Enter applies · Esc closes", x + 28, y + panel_h - 32, 12, kFaint);
}

void App::draw_help(Canvas& canvas) {
    const float width = static_cast<float>(canvas.w());
    const float height = static_cast<float>(canvas.h());
    canvas.clear(rgb(kModalBg));
    canvas.rect(0, 0, width, height, ::Color{0, 0, 0, 0.74f});

    const auto& rows = key_help();
    const float panel_w = std::min(900.0f, width - 60.0f);
    const float row_step = 24.0f;
    const float panel_h =
        std::min(640.0f, 120.0f + static_cast<float>(rows.size()) * row_step);
    const float x = (width - panel_w) * 0.5f;
    const float y = (height - panel_h) * 0.5f;
    canvas.rect(x, y, panel_w, panel_h, rgb(kModalPanel), kRadiusPanel);
    canvas.rectOutline(x, y, panel_w, panel_h, rgb(kBorder), kRadiusPanel);

    put_text("Keyboard shortcuts", x + 28, y + 20, 19, kText, FontStyle::Bold);

    float key_col = 0.0f;
    for (const auto& [keys, description] : rows) {
        (void)description;
        key_col = std::max(key_col, text_.width(keys, 14, FontStyle::Bold));
    }
    const float left = x + 28;
    const float keys_right = left + key_col;
    const float desc_x = keys_right + 30;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const float row_y = y + 64 + static_cast<float>(i) * row_step;
        put_text(rows[i].first, keys_right - text_.width(rows[i].first, 14, FontStyle::Bold),
                 row_y, 14, kAmber, FontStyle::Bold);
        put_text(rows[i].second, desc_x, row_y, 14, kSoft);
    }
    put_text("Esc or ? closes this card", x + 28, y + panel_h - 34, 12, kDim);
}

} // namespace tp
