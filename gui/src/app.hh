#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_view.hh"
#include "layout.hh"
#include "text.hh"
#include "teleprompter/recents.hh"
#include "teleprompter/recorder.hh"
#include "teleprompter/runtime.hh"
#include "teleprompter/script.hh"

class Canvas;
class Host;
class Renderer;

namespace tp {

using gui::ClipRect;
using gui::Glyph;

class App : public AppView {
public:
    explicit App(RuntimeOptions options);
    ~App() override;

    bool create();
    void run();

    // ── AppView ────────────────────────────────────────────────────────────
    void onHostResized() override;
    void onHostLayoutInvalidated() override;
    void onHostExposed() override;
    void onKeyDownPortable(int keyCode) override;
    void onKeyUpPortable(int keyCode) override;
    void onMouseMove(int x, int y) override;
    void onMouseLeave() override;
    void onLButtonDown(int x, int y) override;
    void onLButtonUp(int x, int y) override;
    void onMouseWheel(int x, int y, int delta) override;
    void onTimer(int timerId) override;
    void shutdown() override;

private:
    enum class Page { Library, Prompter };
    enum class RecPhase { Idle, Recording, Finalizing };

    struct Hit {
        std::string id;
        float x = 0, y = 0, w = 0, h = 0;
        bool contains(float px, float py) const {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    struct LibraryEntry {
        std::string path;
        std::string title;
        std::string subtitle;
        std::string meta;
        bool sample = false;
        bool valid = true;
    };

    struct PickerEntry {
        std::string name;
        std::string path;
        bool directory = false;
    };

    // lifecycle / layout
    bool create_host();
    bool create_renderer();
    void compute_font();
    void refresh_library();
    void seed_recents();
    void open_picker();
    void load_picker();
    void picker_activate(int index);
    void picker_up();

    void open_script(const std::string& path, bool remember = true);
    void open_test_script();
    void reload_script(bool notify);
    void go_home();
    void reset_playback();
    void apply_plan(std::vector<Paragraph> plan, std::vector<std::string> warnings);
    void rebuild_layout();
    void rebuild_timeline();

    // play / scroll
    void toggle_play();
    void seek(double position);
    void nudge(double delta);
    void cycle_cue();
    void toggle_fullscreen();
    void toggle_recording();
    void start_recording();
    void stop_recording();
    void finish_recording(int code);
    void tick();
    void update_inhibit();
    void show_message(const std::string& text, bool error = false, double seconds = 6.0);
    bool message_visible() const;

    // draw
    void draw_frame();
    void draw_library(Canvas& canvas);
    void draw_prompter(Canvas& canvas);
    void draw_hud(Canvas& canvas);
    void draw_section(Canvas& canvas);
    void draw_help(Canvas& canvas);
    void draw_picker(Canvas& canvas);
    void draw_cue_bars(Canvas& canvas);
    void draw_progress(Canvas& canvas);
    void draw_rec_frame(Canvas& canvas);
    void draw_script(Canvas& canvas);

    void collect_all_text();
    void sync_font();
    void put_text(std::string_view text, float x, float top, float size, Rgb colour,
                  FontStyle style = FontStyle::Roman, float alpha = 1.0f,
                  bool centered = false);
    void put_text_clipped(std::string_view text, float x, float top, float size, Rgb colour,
                          FontStyle style, float alpha, bool centered,
                          const ClipRect& clip);
    void add_hit(std::string id, float x, float y, float w, float h);
    const Hit* hit_at(float x, float y) const;

    double now_monotonic() const;
    double now_epoch() const;
    std::string state_text() const;
    std::string remaining_text() const;
    std::string theme_text() const;
    bool is_prompter() const { return page_ == Page::Prompter; }

    RuntimeOptions options_;
    std::unique_ptr<Host> host_;
    std::unique_ptr<Renderer> renderer_;
    gui::TextPainter text_;
    std::unique_ptr<Recorder> recorder_;

    Page page_ = Page::Library;
    RecPhase rec_phase_ = RecPhase::Idle;
    bool quit_ = false;
    bool shutdown_done_ = false;
    bool msdf_initialized_ = false;

    // current score
    std::optional<std::string> script_path_;
    bool is_sample_ = false;
    std::vector<Paragraph> plan_;
    std::vector<Word> flat_words_;
    std::vector<double> paragraph_starts_;
    double duration_ = 0.0;
    gui::ScriptLayout layout_;
    Timeline timeline_;
    double total_weight_ = 0.0;
    double w_ = 0.0;
    double anchor_w_ = 0.0;
    double anchor_t_ = 0.0;
    double scroll_ = 0.0;
    bool playing_ = false;
    bool at_end_ = false;
    bool layout_dirty_ = true;
    bool needs_redraw_ = true;
    uint32_t layout_width_ = 0;
    uint32_t layout_height_ = 0;

    // presentation
    float cue_fraction_ = 0.25f;
    float font_px_ = 56.0f;
    float script_px_ = 56.0f;
    bool centered_ = true;
    bool karaoke_ = true;
    bool mood_on_ = true;
    bool hud_visible_ = true;
    bool hud_dimmed_ = false;
    bool help_visible_ = false;
    bool fullscreen_ = false;
    bool ctrl_down_ = false;
    bool shift_down_ = false;
    int cursor_ = -1;
    double last_motion_t_ = 0.0;

    // library / picker
    std::vector<Recent> recents_;
    std::vector<LibraryEntry> library_entries_;
    int library_selected_ = 0;
    float library_scroll_ = 0.0f;
    bool picker_open_ = false;
    std::string picker_dir_;
    std::vector<PickerEntry> picker_entries_;
    int picker_selected_ = 0;
    int picker_scroll_ = 0;

    // status / recording
    std::string message_;
    bool message_error_ = false;
    double message_until_ = 0.0;
    double rec_started_ = 0.0;
    double record_test_stop_ = -1.0;
    bool record_test_ = false;
    double last_repeat_[256] = {};

    std::vector<float> shapes_;
    std::vector<float> text_quads_;
    std::vector<Hit> hits_;
    int frames_drawn_ = 0;
};

} // namespace tp
