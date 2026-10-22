#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tp {

struct Rgb {
    int r = 255, g = 255, b = 255;
};

Rgb parse_hex(std::string_view hex, Rgb fallback = {242, 242, 242}) noexcept;
std::string hex_from_rgb(Rgb c);
Rgb mix(Rgb a, Rgb b, double amount);
Rgb scale(Rgb c, double factor);

// ── The score's colour vocabulary ───────────────────────────────────────────
inline constexpr std::string_view kMoodDefault = "plain";
inline constexpr std::string_view kKaraokeGray = "#7d8591";
inline constexpr std::string_view kKaraokeNeutral = "#f2f2f2";
inline constexpr std::string_view kCommentDim = "#a78c60";
inline constexpr std::string_view kCommentLit = "#f0c27a";
inline constexpr std::string_view kCommentBg = "#2a2114";

Rgb mood_colour(std::string_view name);
Rgb comment_colour(double progress);
double word_fill(double word_clock, double word_seconds, double now);
double character_fill(double progress, int index, int count);

std::string format_time(double seconds);
std::string preview_text(std::string_view text, int width = 70);
std::string human_size(std::uint64_t bytes);
std::string format_age(double opened, double now);
std::string display_path(std::string_view path);

} // namespace tp
