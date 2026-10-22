#pragma once

#include <cstdint>
#include <vector>

#include "text.hh"
#include "teleprompter/script.hh"

namespace tp::gui {

struct ScriptMetrics {
    float width = 1280.0f;
    float height = 720.0f;
    float cue_y = 180.0f;
    float font_px = 56.0f;
    bool centered = true;
};

struct PlacedLine {
    float center = 0.0f;
    float x0 = 0.0f;
    float x1 = 0.0f;
};

struct PlacedWord {
    int index = -1;
    bool comment = false;
    bool direction = false;
    float first_line_center = 0.0f;
    std::vector<PlacedLine> lines;
};

struct PlacedParagraph {
    int index = -1;
    bool direction = false;
    float top = 0.0f;
    float bottom = 0.0f;
    // A comment-only paragraph is drawn as a card; these are its bounds. Zero
    // width/height means the paragraph is a silent beat and has no card.
    float card_x = 0.0f, card_y = 0.0f, card_w = 0.0f, card_h = 0.0f;
    std::vector<PlacedWord> words;
};

struct ScriptLayout {
    std::vector<PlacedParagraph> paragraphs;
    std::vector<Glyph> glyphs;
    std::vector<float> word_center;
    std::vector<float> word_seconds;
    std::vector<PlacedWord> words;
    float max_scroll = 0.0f;
    float column_left = 0.0f;
    float column_width = 0.0f;
    float script_px = 56.0f;
    float direction_px = 38.0f;
};

ScriptLayout build_script_layout(const std::vector<Paragraph>& plan, TextPainter& painter,
                                 const ScriptMetrics& metrics);

int utf8_codepoint_count(std::string_view text);

} // namespace tp::gui
