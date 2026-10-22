#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tp {

// ── The timing score ────────────────────────────────────────────────────────
//
// Every displayed word and comment carries a positive [seconds] prefix. The
// parser strips the prefixes, records each token's clock, and sums the total.
// Nothing is estimated or rescaled: a missing time is a hard error with a
// source line, and the window refuses the score.

struct Word {
    std::string text;          // display text, markup stars removed
    double seconds = 0.0;
    std::string mood;          // mood name or #rrggbb
    std::string theme;
    int char_start = 0;        // codepoint offset into Paragraph::text
    int byte_start = 0;        // UTF-8 byte offset into Paragraph::text
    double clock = 0.0;        // absolute script clock at this token
    bool is_comment = false;
    bool bold = false;
    bool italic = false;
};

struct Paragraph {
    std::string text;          // display text, markup stars removed
    bool is_direction = false; // no spoken words: standalone comment / silent beat
    double seconds = 0.0;
    std::string mood;
    std::string theme;
    std::vector<Word> words;
    double start = 0.0;
    std::string markup;
};

struct TimingIssue {
    int line = 0;
    int paragraph = 0;
    std::string kind;
    std::string text;
    std::string reason;

    std::string describe() const;
};

class ScriptTimingError : public std::runtime_error {
public:
    explicit ScriptTimingError(std::vector<TimingIssue> issues);
    const std::vector<TimingIssue>& issues() const { return issues_; }
    std::string summary() const;

private:
    std::vector<TimingIssue> issues_;
};

struct SectionState {
    int index = -1;
    const Paragraph* paragraph = nullptr;
    double elapsed = 0.0;
    double remaining = 0.0;
    double progress = 0.0;
};

inline constexpr double kWordMaxSeconds = 600.0;
inline constexpr int kTimingIssuesPrinted = 20;

std::vector<Paragraph> paragraph_plan(std::string_view text,
                                      std::vector<std::string>* warnings = nullptr);
std::vector<std::string> split_paragraphs(std::string_view text);

// Pango-shaped markup is retained for tooling/tests; the Vulkan UI uses the
// plain text plus Word::bold/italic instead of parsing markup again.
std::string to_markup(std::string_view text);
std::string section_title(const Paragraph& paragraph, int width = 46);
std::optional<SectionState> section_state(const std::vector<Paragraph>& plan,
                                          double now,
                                          const std::vector<double>* starts = nullptr);

std::optional<std::string> script_read_time(std::string_view text);
std::optional<std::string> read_script_time(const std::string& path);
int print_timing(const std::string& path, bool show_words);

std::string sample_script_text();
std::string default_script_path();
std::string test_script_path();

// ── Weight ↔ pixel mapping ──────────────────────────────────────────────────
//
// A knot is a scroll position at a word boundary; cumulative weights are the
// clock spent before that knot. Repeated positions are legal and mean one line
// dwelling for several words.
class Timeline {
public:
    void clear(double total_weight = 0.0);
    void rebuild_entries(const std::vector<std::pair<double, double>>& entries,
                         double max_scroll);
    double pos_from_weight(double weight) const;
    double weight_from_pos(double position) const;

    const std::vector<double>& knots() const { return knots_; }
    const std::vector<double>& cum() const { return cum_; }
    double total_weight() const { return total_weight_; }

private:
    std::vector<double> knots_{0.0};
    std::vector<double> cum_{0.0};
    double total_weight_ = 0.0;
};

} // namespace tp
