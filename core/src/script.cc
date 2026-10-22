#include "teleprompter/script.hh"
#include "teleprompter/colour.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

#ifndef TP_SAMPLE_PATH
#define TP_SAMPLE_PATH ""
#endif

namespace tp {
namespace {

constexpr std::string_view kMoodNames[] = {"somber", "quiet", "plain", "warm", "bright"};

struct Token {
    std::string text;
    double seconds = 0.0;
    std::string mood;
    std::string theme;
    bool is_comment = false;
    bool is_silence = false;
};

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool is_hex_colour(std::string_view value) {
    if (value.size() != 7 || value[0] != '#') {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

bool known_mood(std::string_view value) {
    for (std::string_view mood : kMoodNames) {
        if (value == mood) {
            return true;
        }
    }
    return false;
}

std::string strip_stars(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c != '*') {
            out.push_back(c);
        }
    }
    return out;
}

bool has_bold_markup(std::string_view value) {
    return value.find("**") != std::string_view::npos;
}

bool has_italic_markup(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '*') {
            continue;
        }
        if (i + 1 < value.size() && value[i + 1] == '*') {
            ++i;
            continue;
        }
        return true;
    }
    return false;
}

int utf8_count(std::string_view value) {
    int count = 0;
    for (unsigned char c : value) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}


std::regex duration_regex() {
    static const std::regex re(R"(^\[(\d+(?:\.\d+)?)\]$)");
    return re;
}

std::regex legacy_anchor_regex() {
    static const std::regex re(R"(^\[(\d{1,3}(?::\d{1,2}){1,2}(?:\.\d+)?)\]$)");
    return re;
}


std::string collapse_spaces(std::string_view value) {
    std::string out;
    bool space = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out.push_back(' ');
        }
        space = false;
        out.push_back(c);
    }
    return out;
}

std::string escape_markup(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}


} // namespace

// ── Colour helpers ──────────────────────────────────────────────────────────
Rgb parse_hex(std::string_view hex, Rgb fallback) {
    if (hex.size() != 7 || hex[0] != '#') {
        return fallback;
    }
    auto byte = [&](std::size_t at) {
        return static_cast<int>(std::stoi(std::string(hex.substr(at, 2)), nullptr, 16));
    };
    try {
        return Rgb{byte(1), byte(3), byte(5)};
    } catch (...) {
        return fallback;
    }
}

std::string hex_from_rgb(Rgb c) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
                  std::max(0, std::min(255, c.r)),
                  std::max(0, std::min(255, c.g)),
                  std::max(0, std::min(255, c.b)));
    return buffer;
}

Rgb mix(Rgb a, Rgb b, double amount) {
    amount = std::max(0.0, std::min(1.0, amount));
    auto channel = [amount](int x, int y) {
        return static_cast<int>(std::lround(x + (y - x) * amount));
    };
    return Rgb{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b)};
}

Rgb scale(Rgb c, double factor) {
    auto channel = [factor](int value) {
        return std::max(0, std::min(255, static_cast<int>(std::lround(value * factor))));
    };
    return Rgb{channel(c.r), channel(c.g), channel(c.b)};
}

Rgb mood_colour(std::string_view name) {
    const std::string lowered = lower_ascii(std::string(name));
    if (lowered == "somber") return parse_hex("#9fb0c4");
    if (lowered == "quiet") return parse_hex("#c3ccd6");
    if (lowered == "plain") return parse_hex("#f2f2f2");
    if (lowered == "warm") return parse_hex("#f5dcc0");
    if (lowered == "bright") return parse_hex("#ffe9a8");
    if (is_hex_colour(name)) return parse_hex(name);
    return parse_hex("#f2f2f2");
}

Rgb comment_colour(double progress) {
    return mix(parse_hex(kCommentDim), parse_hex(kCommentLit), progress);
}

double word_fill(double word_clock, double word_seconds, double now) {
    if (word_seconds <= 0.0) {
        return now >= word_clock ? 1.0 : 0.0;
    }
    return std::max(0.0, std::min(1.0, (now - word_clock) / word_seconds));
}

double character_fill(double progress, int index, int count) {
    if (count <= 0) {
        return std::max(0.0, std::min(1.0, progress));
    }
    const double edge = std::max(0.0, std::min(1.0, progress)) * count;
    return std::max(0.0, std::min(1.0, edge - index));
}

std::string format_time(double seconds) {
    const long total = std::max(0L, static_cast<long>(std::llround(seconds)));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", total / 60, total % 60);
    return buffer;
}

std::string preview_text(std::string_view text, int width) {
    std::string flat;
    bool space = false;
    for (char c : text) {
        if (c == '*') {
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !flat.empty()) {
            flat.push_back(' ');
        }
        space = false;
        flat.push_back(c);
    }
    if (static_cast<int>(flat.size()) <= width) {
        return flat;
    }
    return flat.substr(0, static_cast<std::size_t>(std::max(0, width - 1))) + "\u2026";
}

std::string human_size(std::uint64_t bytes) {
    double size = static_cast<double>(bytes);
    const char* units[] = {"B", "KB", "MB", "GB"};
    for (int i = 0; i < 4; ++i) {
        if (size < 1024.0 || i == 3) {
            char buffer[64];
            if (i == 0) {
                std::snprintf(buffer, sizeof(buffer), "%.0f %s", size, units[i]);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[i]);
            }
            return buffer;
        }
        size /= 1024.0;
    }
    return "0 B";
}

std::string format_age(double opened, double now) {
    const double delta = std::max(0.0, now - opened);
    char buffer[64];
    if (delta < 60.0) return "just now";
    if (delta < 3600.0) {
        std::snprintf(buffer, sizeof(buffer), "%d min ago", static_cast<int>(delta / 60.0));
        return buffer;
    }
    if (delta < 86400.0) {
        std::snprintf(buffer, sizeof(buffer), "%d h ago", static_cast<int>(delta / 3600.0));
        return buffer;
    }
    if (delta < 7.0 * 86400.0) {
        const int days = static_cast<int>(delta / 86400.0);
        std::snprintf(buffer, sizeof(buffer), "%d day%s ago", days, days == 1 ? "" : "s");
        return buffer;
    }
    std::time_t stamp = static_cast<std::time_t>(opened);
    std::tm local{};
    localtime_r(&stamp, &local);
    std::strftime(buffer, sizeof(buffer), "%b %d, %Y", &local);
    std::string out(buffer);
    const std::size_t zero = out.find(" 0");
    if (zero != std::string::npos) {
        out.erase(zero, 1);
    }
    return out;
}

std::string display_path(std::string_view path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(std::string(path)), ec);
    if (ec) p = fs::path(std::string(path));
    const char* home = std::getenv("HOME");
    if (home != nullptr && !*home) home = nullptr;
    if (home != nullptr) {
        fs::path h(home);
        auto relative = p.lexically_relative(h);
        if (!relative.empty() && relative.native().rfind("..", 0) != 0) {
            return relative == "." ? "~" : "~/" + relative.generic_string();
        }
    }
    return p.generic_string();
}

std::string TimingIssue::describe() const {
    std::ostringstream out;
    out << "line " << line << ": " << kind << " '" << text << "' " << reason;
    return out.str();
}

namespace {
std::string describe_all(const std::vector<TimingIssue>& issues) {
    if (issues.empty()) return "incomplete timing";
    std::ostringstream joined;
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i != 0) joined << '\n';
        joined << issues[i].describe();
    }
    return joined.str();
}
} // namespace

ScriptTimingError::ScriptTimingError(std::vector<TimingIssue> issues)
    : std::runtime_error(describe_all(issues)), issues_(std::move(issues)) {}

std::string ScriptTimingError::summary() const {
    if (issues_.empty()) return "incomplete timing";
    std::string out = issues_.front().describe();
    if (issues_.size() > 1) {
        out += " (+" + std::to_string(issues_.size() - 1) + " more)";
    }
    return out;
}

namespace {

std::vector<std::pair<std::string, int>> paragraph_blocks(std::string_view text) {
    std::vector<std::pair<std::string, int>> blocks;
    std::size_t start = 0;
    std::size_t i = 0;
    while (i + 1 < text.size()) {
        if (text[i] == '\n') {
            std::size_t j = i + 1;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])) &&
                   text[j] != '\n') {
                ++j;
            }
            if (j < text.size() && text[j] == '\n') {
                const std::string raw(text.substr(start, i - start));
                const std::string body = trim(raw);
                if (!body.empty()) {
                    const std::size_t offset = start + raw.find(body);
                    int line = 1;
                    for (std::size_t k = 0; k < offset; ++k) {
                        if (text[k] == '\n') ++line;
                    }
                    blocks.emplace_back(body, line);
                }
                start = j + 1;
                i = start;
                continue;
            }
        }
        ++i;
    }
    const std::string raw(text.substr(start));
    const std::string body = trim(raw);
    if (!body.empty()) {
        const std::size_t offset = start + raw.find(body);
        int line = 1;
        for (std::size_t k = 0; k < offset; ++k) {
            if (text[k] == '\n') ++line;
        }
        blocks.emplace_back(body, line);
    }
    return blocks;
}

struct ParseState {
    std::string mood = std::string(kMoodDefault);
    std::string theme;
};

void add_issue(std::vector<TimingIssue>& issues, std::string_view body, std::size_t start,
               int first_line, int paragraph, const std::string& kind,
               const std::string& text, const std::string& reason) {
    int line = first_line;
    for (std::size_t i = 0; i < start && i < body.size(); ++i) {
        if (body[i] == '\n') ++line;
    }
    issues.push_back(TimingIssue{line, paragraph, kind, text, reason});
}

std::vector<Token> parse_tokens(std::string_view raw, ParseState& state,
                                std::vector<std::string>* warnings,
                                std::vector<TimingIssue>& issues,
                                int paragraph, int first_line) {
    const std::string body = trim(raw);
    std::vector<Token> tokens;
    std::optional<double> pending;
    std::size_t i = 0;
    const std::size_t end = body.size();

    auto issue = [&](std::size_t start, const std::string& kind, const std::string& text,
                     const std::string& reason) {
        add_issue(issues, body, start, first_line, paragraph, kind, text, reason);
    };
    auto take_pending = [&](std::size_t start, const std::string& kind,
                            const std::string& text) {
        const double seconds = pending.value_or(0.0);
        const bool had = pending.has_value();
        pending.reset();
        if (!had) {
            issue(start, kind, text, "has no [seconds] time");
            return 0.0;
        }
        if (seconds <= 0.0) {
            issue(start, kind, text,
                  "has zero duration; use a positive [seconds] time");
            return 0.0;
        }
        return seconds;
    };

    // Whole-paragraph wrapped forms, kept for the old direction spelling.
    static const std::regex wrapped_timed(
        R"(^\s*\[(\d+(?:\.\d+)?)\]\s*\*{0,2}\s*\[([^\[\]]*)\]\s*\*{0,2}\s*$)");
    static const std::regex wrapped_comment(
        R"(^\s*\*{0,2}\s*\[([^\[\]]*)\]\s*\*{0,2}\s*$)");
    std::smatch match;
    if (std::regex_match(body, match, wrapped_timed)) {
        const std::string text = "[" + trim(match[2].str()) + "]";
        const double seconds = std::max(0.0, std::min(kWordMaxSeconds, std::stod(match[1].str())));
        if (seconds <= 0.0) {
            issue(0, "comment", text, "has zero duration; use a positive [seconds] time");
        }
        tokens.push_back(Token{text, seconds, state.mood, state.theme, true, false});
        return tokens;
    }
    if (std::regex_match(body, legacy_anchor_regex())) {
        issue(0, "anchor", body, "is no longer valid; use a positive [seconds] prefix");
        return tokens;
    }
    if (std::regex_match(body, match, wrapped_comment) &&
        !std::regex_match(body, duration_regex())) {
        const std::string text = "[" + trim(match[1].str()) + "]";
        issue(0, "comment", text, "has no [seconds] time");
        tokens.push_back(Token{text, 0.0, state.mood, state.theme, true, false});
        return tokens;
    }

    while (i < end) {
        if (std::isspace(static_cast<unsigned char>(body[i]))) {
            ++i;
            continue;
        }

        if (body[i] == '{') {
            const std::size_t close = body.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string content = body.substr(i + 1, close - i - 1);
                const std::size_t colon = content.find(':');
                const std::string name = lower_ascii(trim(content.substr(0, colon)));
                const std::string theme = colon == std::string::npos
                    ? std::string()
                    : trim(content.substr(colon + 1));
                if (known_mood(name) || is_hex_colour(name)) {
                    state.mood = name;
                    if (!theme.empty()) state.theme = theme;
                    i = close + 1;
                    continue;
                }
                if (warnings != nullptr) {
                    std::string expected;
                    for (std::string_view mood : kMoodNames) {
                        if (!expected.empty()) expected += ", ";
                        expected += mood;
                    }
                    warnings->push_back("unknown mood {" + trim(content) +
                                        "} — use " + expected + " or #rrggbb");
                }
                const std::size_t start = i;
                const std::string text = body.substr(i, close - i + 1);
                const double seconds = take_pending(start, "word", text);
                tokens.push_back(Token{text, seconds, state.mood, state.theme, false, false});
                i = close + 1;
                continue;
            }
        }

        if (body[i] == '*') {
            static const std::regex wrapped_inline(R"(\*{1,2}\s*\[([^\[\]]*)\]\s*\*{1,2})");
            std::smatch inline_match;
            const auto search_begin = body.cbegin() + static_cast<std::ptrdiff_t>(i);
            if (std::regex_search(search_begin, body.cend(), inline_match, wrapped_inline) &&
                inline_match.position() == 0) {
                const std::string text = "[" + trim(inline_match[1].str()) + "]";
                const double seconds = take_pending(i, "comment", text);
                tokens.push_back(Token{text, seconds, state.mood, state.theme, true, false});
                i += static_cast<std::size_t>(inline_match.length());
                continue;
            }
        }

        if (body[i] == '[') {
            const std::size_t close = body.find(']', i + 1);
            if (close != std::string::npos) {
                const std::string raw_token = body.substr(i, close - i + 1);
                std::smatch duration_match;
                if (std::regex_match(raw_token, duration_match, duration_regex())) {
                    pending = std::max(0.0, std::min(kWordMaxSeconds,
                                                     std::stod(duration_match[1].str())));
                    i = close + 1;
                    continue;
                }
                if (std::regex_match(raw_token, legacy_anchor_regex())) {
                    issue(i, "anchor", raw_token,
                          "is no longer valid; use a positive [seconds] prefix");
                    i = close + 1;
                    continue;
                }
                const std::string text = "[" + trim(body.substr(i + 1, close - i - 1)) + "]";
                const double seconds = take_pending(i, "comment", text);
                tokens.push_back(Token{text, seconds, state.mood, state.theme, true, false});
                i = close + 1;
                continue;
            }
        }

        const std::size_t start = i;
        while (i < end && !std::isspace(static_cast<unsigned char>(body[i]))) {
            ++i;
        }
        if (i > start) {
            const std::string text = body.substr(start, i - start);
            const double seconds = take_pending(start, "word", text);
            tokens.push_back(Token{text, seconds, state.mood, state.theme, false, false});
        }
    }

    if (pending.has_value()) {
        tokens.push_back(Token{"", *pending, state.mood, state.theme, false, true});
    }
    return tokens;
}

Paragraph finish_paragraph(const std::vector<Token>& tokens, double clock) {
    std::vector<std::string> raw_parts;
    for (const Token& token : tokens) {
        if (!token.text.empty()) raw_parts.push_back(token.text);
    }
    std::string raw_display;
    for (std::size_t i = 0; i < raw_parts.size(); ++i) {
        if (i != 0) raw_display.push_back(' ');
        raw_display += raw_parts[i];
    }
    if (raw_display.empty()) raw_display = " ";

    const std::string plain_display = strip_stars(raw_display);
    std::vector<Word> words;
    int cursor_chars = 0;
    std::size_t cursor_bytes = 0;
    double running = clock;

    for (const Token& token : tokens) {
        const std::string plain_word = strip_stars(token.text);
        int char_start = cursor_chars;
        int byte_start = static_cast<int>(cursor_bytes);
        if (!plain_word.empty()) {
            const std::size_t found = plain_display.find(plain_word, cursor_bytes);
            if (found != std::string::npos) {
                char_start = utf8_count(std::string_view(plain_display).substr(0, found));
                byte_start = static_cast<int>(found);
            }
        }
        Word word;
        word.text = plain_word;
        word.seconds = token.seconds;
        word.mood = token.mood;
        word.theme = token.theme;
        word.char_start = char_start;
        word.byte_start = byte_start;
        word.clock = running;
        word.is_comment = token.is_comment;
        word.bold = has_bold_markup(token.text);
        word.italic = has_italic_markup(token.text);
        words.push_back(std::move(word));
        running += token.seconds;
        if (!plain_word.empty()) {
            cursor_chars = char_start + utf8_count(plain_word) + 1;
            cursor_bytes = static_cast<std::size_t>(byte_start) + plain_word.size() + 1;
        }
    }

    Paragraph paragraph;
    paragraph.text = plain_display;
    paragraph.is_direction = true;
    for (const Word& word : words) {
        if (!word.text.empty() && !word.is_comment) {
            paragraph.is_direction = false;
            break;
        }
    }
    paragraph.seconds = running - clock;
    const Word* first = nullptr;
    for (const Word& word : words) {
        if (!word.text.empty() && !word.is_comment) {
            first = &word;
            break;
        }
    }
    if (first == nullptr && !words.empty()) first = &words.front();
    paragraph.mood = first != nullptr ? first->mood : std::string(kMoodDefault);
    paragraph.theme = first != nullptr ? first->theme : std::string();
    paragraph.words = std::move(words);
    paragraph.start = clock;
    paragraph.markup = to_markup(raw_display);
    return paragraph;
}

} // namespace

std::vector<Paragraph> paragraph_plan(std::string_view text,
                                      std::vector<std::string>* warnings) {
    std::vector<TimingIssue> issues;
    ParseState state;
    std::vector<Paragraph> plan;
    double clock = 0.0;
    int number = 0;
    for (const auto& [body, first_line] : paragraph_blocks(text)) {
        ++number;
        std::vector<Token> tokens =
            parse_tokens(body, state, warnings, issues, number, first_line);
        if (tokens.empty()) {
            continue; // mood-only header
        }
        Paragraph paragraph = finish_paragraph(tokens, clock);
        clock += paragraph.seconds;
        plan.push_back(std::move(paragraph));
    }
    if (!issues.empty()) {
        throw ScriptTimingError(std::move(issues));
    }
    return plan;
}

std::vector<std::string> split_paragraphs(std::string_view text) {
    std::vector<std::string> out;
    for (const auto& block : paragraph_blocks(text)) {
        out.push_back(block.first);
    }
    return out;
}

std::string to_markup(std::string_view text) {
    const std::string escaped = escape_markup(text);
    std::string out;
    out.reserve(escaped.size() + 16);
    for (std::size_t i = 0; i < escaped.size();) {
        if (escaped.compare(i, 2, "**") == 0) {
            const std::size_t close = escaped.find("**", i + 2);
            if (close != std::string::npos) {
                out += "<b>";
                out.append(escaped, i + 2, close - i - 2);
                out += "</b>";
                i = close + 2;
                continue;
            }
        }
        if (escaped[i] == '*') {
            const std::size_t close = escaped.find('*', i + 1);
            if (close != std::string::npos && (close + 1 >= escaped.size() || escaped[close + 1] != '*')) {
                out += "<i>";
                out.append(escaped, i + 1, close - i - 1);
                out += "</i>";
                i = close + 1;
                continue;
            }
        }
        out.push_back(escaped[i]);
        ++i;
    }
    return out;
}

std::string section_title(const Paragraph& paragraph, int width) {
    std::string text;
    for (char c : paragraph.text) {
        if (c == '[' || c == ']' || c == '*') {
            text.push_back(' ');
        } else {
            text.push_back(c);
        }
    }
    text = collapse_spaces(text);
    if (text.empty()) return "Hold";
    if (static_cast<int>(text.size()) <= width) return text;
    return text.substr(0, static_cast<std::size_t>(std::max(0, width - 1))) + "\u2026";
}

std::optional<SectionState> section_state(const std::vector<Paragraph>& plan, double now,
                                          const std::vector<double>* starts) {
    if (plan.empty()) return std::nullopt;
    std::vector<double> local;
    if (starts == nullptr) {
        local.reserve(plan.size());
        for (const Paragraph& paragraph : plan) local.push_back(paragraph.start);
        starts = &local;
    }
    auto it = std::upper_bound(starts->begin(), starts->end(), now);
    int index = static_cast<int>(it - starts->begin()) - 1;
    index = std::max(0, std::min(index, static_cast<int>(plan.size()) - 1));
    const Paragraph& paragraph = plan[static_cast<std::size_t>(index)];
    if (!paragraph.is_direction) return std::nullopt;
    const double elapsed = std::max(0.0, std::min(paragraph.seconds, now - paragraph.start));
    SectionState state;
    state.index = index;
    state.paragraph = &paragraph;
    state.elapsed = elapsed;
    state.remaining = std::max(0.0, paragraph.seconds - elapsed);
    state.progress = paragraph.seconds > 0.0 ? elapsed / paragraph.seconds : 1.0;
    return state;
}

std::optional<std::string> script_read_time(std::string_view text) {
    try {
        const std::vector<Paragraph> plan = paragraph_plan(text);
        double total = 0.0;
        for (const Paragraph& paragraph : plan) total += paragraph.seconds;
        return format_time(total);
    } catch (const ScriptTimingError&) {
        return std::nullopt;
    }
}

std::optional<std::string> read_script_time(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return script_read_time(buffer.str());
}

int print_timing(const std::string& path, bool show_words) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    std::vector<std::string> warnings;
    std::vector<Paragraph> plan;
    try {
        plan = paragraph_plan(text, &warnings);
    } catch (const ScriptTimingError& error) {
        std::fprintf(stderr, "%s: timing is incomplete\n", path.c_str());
        const auto& issues = error.issues();
        for (std::size_t i = 0; i < issues.size() && i < static_cast<std::size_t>(kTimingIssuesPrinted); ++i) {
            std::fprintf(stderr, " %s\n", issues[i].describe().c_str());
        }
        if (issues.size() > static_cast<std::size_t>(kTimingIssuesPrinted)) {
            std::fprintf(stderr, " … and %zu more timing issue(s)\n",
                         issues.size() - static_cast<std::size_t>(kTimingIssuesPrinted));
        }
        for (const std::string& warning : warnings) {
            std::fprintf(stderr, " warning: %s\n", warning.c_str());
        }
        return 1;
    }
    if (plan.empty()) {
        std::printf("%s: no paragraphs to time\n", path.c_str());
        return 0;
    }

    int spoken = 0;
    int comments = 0;
    double total = 0.0;
    for (const Paragraph& paragraph : plan) {
        total += paragraph.seconds;
        for (const Word& word : paragraph.words) {
            if (word.is_comment) ++comments;
            else if (!word.text.empty()) ++spoken;
        }
    }
    std::printf("%s — %zu paragraphs, %d words, %d comments, total %s (%.1fs)\n\n",
                path.c_str(), plan.size(), spoken, comments, format_time(total).c_str(), total);
    std::printf("   start   time  kind  mood     paragraph\n");
    for (const Paragraph& paragraph : plan) {
        std::string body = preview_text(paragraph.text);
        if (!paragraph.theme.empty()) body = paragraph.theme + ": " + body;
        std::printf(" %7s  %5.1fs  %s  %-7s  %s\n",
                    format_time(paragraph.start).c_str(), paragraph.seconds,
                    paragraph.is_direction ? "note" : "say ", paragraph.mood.c_str(),
                    body.c_str());
    }
    if (show_words) {
        std::printf("\n   start   time  kind   mood     word\n");
        for (const Paragraph& paragraph : plan) {
            for (const Word& word : paragraph.words) {
                const char* kind = word.is_comment ? "note " : (word.text.empty() ? "pause" : "word ");
                const std::string label = word.text.empty() ? "—" : preview_text(word.text, 48);
                std::printf(" %7s  %5.2fs  %s  %-7s  %s\n",
                            format_time(word.clock).c_str(), word.seconds, kind,
                            word.mood.c_str(), label.c_str());
            }
        }
    }
    std::printf("\n total %s  (%.1f seconds, %d spoken words)\n",
                format_time(total).c_str(), total, spoken);
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
    return 0;
}

namespace {
constexpr std::string_view kTestFallback =
    "{warm}[0.45]The [0.35]built-in [0.35]test [0.30]script [0.35]is [0.30]unavailable.\n\n"
    "[0.35]Open [0.30]one [0.30]of [0.25]your [0.30]own [0.35]scores [0.30]from [0.25]the [0.35]library.\n";
} // namespace

namespace {
std::string exe_dir_hint() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return exe.parent_path().generic_string();
}
} // namespace

std::string sample_script_text() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if constexpr (std::string_view(TP_SAMPLE_PATH).size() > 0) {
        if (fs::is_regular_file(TP_SAMPLE_PATH, ec)) {
            std::ifstream in(TP_SAMPLE_PATH, std::ios::binary);
            if (in) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                return buffer.str();
            }
        }
    }
    const std::string beside_exe = exe_dir_hint() + "/samples/test-script.txt";
    for (const std::string& candidate : {beside_exe, std::string("samples/test-script.txt"),
                                         std::string("../samples/test-script.txt")}) {
        if (fs::is_regular_file(candidate, ec)) {
            std::ifstream in(candidate, std::ios::binary);
            if (in) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                return buffer.str();
            }
        }
    }
    return std::string(kTestFallback);
}

std::string default_script_path() {
    std::error_code ec;
    const std::string beside_exe = exe_dir_hint() + "/script.txt";
    if (std::filesystem::is_regular_file(beside_exe, ec)) return beside_exe;
    return "script.txt";
}

std::string test_script_path() {
    return "samples/test-script.txt";
}

void Timeline::clear(double total_weight) {
    knots_ = {0.0};
    cum_ = {0.0};
    total_weight_ = total_weight;
}

void Timeline::rebuild_entries(const std::vector<std::pair<double, double>>& entries,
                               double max_scroll) {
    if (entries.empty()) {
        clear();
        return;
    }
    total_weight_ = 0.0;
    knots_ = {0.0};
    cum_ = {0.0};
    double previous = 0.0;
    for (const auto& [raw_position, raw_seconds] : entries) {
        const double seconds = std::max(0.0, raw_seconds);
        const double position = std::max(previous, std::min(raw_position, max_scroll));
        total_weight_ += seconds;
        knots_.push_back(position);
        cum_.push_back(total_weight_);
        previous = position;
    }
}

double Timeline::pos_from_weight(double weight) const {
    if (knots_.size() < 2) return 0.0;
    if (weight <= 0.0) return 0.0;
    const auto it = std::upper_bound(cum_.begin(), cum_.end(), weight);
    int index = static_cast<int>(it - cum_.begin()) - 1;
    index = std::max(0, std::min(index, static_cast<int>(cum_.size()) - 2));
    const double w0 = cum_[static_cast<std::size_t>(index)];
    const double w1 = cum_[static_cast<std::size_t>(index + 1)];
    const double p0 = knots_[static_cast<std::size_t>(index)];
    const double p1 = knots_[static_cast<std::size_t>(index + 1)];
    if (w1 <= w0) return p1;
    return p0 + (std::min(weight, w1) - w0) / (w1 - w0) * (p1 - p0);
}

double Timeline::weight_from_pos(double position) const {
    if (knots_.size() < 2) return 0.0;
    if (position <= knots_.front()) return cum_.front();
    if (position >= knots_.back()) return cum_.back();
    const auto it = std::lower_bound(knots_.begin(), knots_.end(), position);
    const std::size_t index = static_cast<std::size_t>(it - knots_.begin());
    if (knots_[index] == position) return cum_[index];
    const double p0 = knots_[index - 1], p1 = knots_[index];
    const double w0 = cum_[index - 1], w1 = cum_[index];
    if (p1 <= p0) return w1;
    return w0 + (position - p0) / (p1 - p0) * (w1 - w0);
}

} // namespace tp
