#include "layout.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "utf8.hh"

namespace tp::gui {
namespace {

struct Atom {
    int word = -1;
    int char_offset = 0;
    std::string text;
    FontStyle style = FontStyle::Roman;
    float width = 0.0f;
    bool space = false;
};

struct Line {
    std::vector<Atom> atoms;
    float width = 0.0f;
};

float line_height_for(const Paragraph& paragraph, float font_px, const TextPainter& painter) {
    if (paragraph.is_direction) {
        const float size = std::max(14.0f, std::round(font_px * 0.68f));
        return std::max(size * 1.3f, painter.line_height(size));
    }
    return painter.line_height(font_px);
}


std::vector<Atom> atoms_for_word(const Paragraph& paragraph, const Word& word, int index,
                                 TextPainter& painter, float size, float max_width) {
    std::vector<Atom> atoms;
    if (word.text.empty()) return atoms;
    const FontStyle style = paragraph.is_direction
        ? FontStyle::Math
        : (word.bold ? FontStyle::Bold : (word.italic ? FontStyle::Italic : FontStyle::Roman));

    // Split at spaces first: a comment is one timed token but may wrap like a
    // sentence. Spaces remain atoms so the line builder can trim them.
    int char_offset = 0;
    std::size_t start = 0;
    while (start < word.text.size()) {
        std::size_t end = start;
        const bool space = std::isspace(static_cast<unsigned char>(word.text[start]));
        while (end < word.text.size() &&
               (static_cast<bool>(std::isspace(static_cast<unsigned char>(word.text[end]))) == space)) {
            ++end;
        }
        const std::string piece = word.text.substr(start, end - start);
        Atom atom;
        atom.word = index;
        atom.char_offset = char_offset;
        atom.text = piece;
        atom.style = style;
        atom.space = space;
        atom.width = painter.width(piece, size, style);
        atoms.push_back(std::move(atom));
        char_offset += utf8_codepoint_count(piece);
        start = end;
    }

    // A single token wider than the column breaks at codepoint boundaries.
    std::vector<Atom> fitted;
    for (const Atom& atom : atoms) {
        if (atom.space || atom.width <= max_width || max_width <= 1.0f) {
            fitted.push_back(atom);
            continue;
        }
        std::string piece;
        int piece_offset = atom.char_offset;
        std::size_t i = 0;
        while (i < atom.text.size()) {
            const std::size_t begin = i;
            const uint32_t cp = utf8::nextCodepoint(atom.text, i);
            (void)cp;
            std::string candidate = piece + atom.text.substr(begin, i - begin);
            if (!piece.empty() && painter.width(candidate, size, atom.style) > max_width) {
                Atom chunk;
                chunk.word = atom.word;
                chunk.char_offset = piece_offset;
                chunk.text = piece;
                chunk.style = atom.style;
                chunk.width = painter.width(piece, size, atom.style);
                fitted.push_back(std::move(chunk));
                piece_offset += utf8_codepoint_count(piece);
                piece = atom.text.substr(begin, i - begin);
            } else {
                piece = std::move(candidate);
            }
        }
        if (!piece.empty()) {
            Atom chunk;
            chunk.word = atom.word;
            chunk.char_offset = piece_offset;
            chunk.text = piece;
            chunk.style = atom.style;
            chunk.width = painter.width(piece, size, atom.style);
            fitted.push_back(std::move(chunk));
        }
    }
    return fitted;
}

void trim_line(Line& line) {
    while (!line.atoms.empty() && line.atoms.back().space) {
        line.width -= line.atoms.back().width;
        line.atoms.pop_back();
    }
}

std::vector<Line> wrap_atoms(const std::vector<Atom>& atoms, float max_width) {
    std::vector<Line> lines;
    Line current;
    for (const Atom& atom : atoms) {
        if (atom.space) {
            if (current.atoms.empty()) continue;
            current.atoms.push_back(atom);
            current.width += atom.width;
            continue;
        }
        if (!current.atoms.empty() && current.width + atom.width > max_width) {
            trim_line(current);
            if (!current.atoms.empty()) lines.push_back(std::move(current));
            current = Line{};
        }
        if (atom.width > max_width && current.atoms.empty()) {
            // The splitter normally prevents this; keep a long atom on its own
            // line rather than dropping it.
        }
        current.atoms.push_back(atom);
        current.width += atom.width;
    }
    trim_line(current);
    if (!current.atoms.empty()) lines.push_back(std::move(current));
    return lines;
}

PlacedWord& word_slot(std::vector<PlacedWord>& words, int index, bool comment, bool direction) {
    if (static_cast<std::size_t>(index) >= words.size()) words.resize(static_cast<std::size_t>(index) + 1);
    PlacedWord& word = words[static_cast<std::size_t>(index)];
    word.index = index;
    word.comment = comment;
    word.direction = direction;
    return word;
}

void add_bounds(PlacedWord& placed, float line_center, float x0, float x1) {
    for (PlacedLine& line : placed.lines) {
        if (std::fabs(line.center - line_center) < 0.01f) {
            line.x0 = std::min(line.x0, x0);
            line.x1 = std::max(line.x1, x1);
            return;
        }
    }
    placed.lines.push_back(PlacedLine{line_center, x0, x1});
    if (placed.first_line_center == 0.0f) placed.first_line_center = line_center;
}

} // namespace

int utf8_codepoint_count(std::string_view text) {
    int count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        (void)utf8::nextCodepoint(text, i);
        ++count;
    }
    return count;
}

ScriptLayout build_script_layout(const std::vector<Paragraph>& plan, TextPainter& painter,
                                 const ScriptMetrics& metrics) {
    ScriptLayout layout;
    layout.script_px = metrics.font_px;
    layout.direction_px = std::max(14.0f, std::round(metrics.font_px * 0.68f));
    layout.column_width = std::min(metrics.width * 0.66f,
                                   std::max(metrics.font_px * 8.0f, metrics.width - 2.0f * metrics.font_px));
    layout.column_left = (metrics.width - layout.column_width) * 0.5f;

    int total_words = 0;
    for (const Paragraph& paragraph : plan) total_words += static_cast<int>(paragraph.words.size());
    layout.word_center.assign(static_cast<std::size_t>(total_words), 0.0f);
    layout.word_seconds.assign(static_cast<std::size_t>(total_words), 0.0f);
    layout.words.reserve(static_cast<std::size_t>(total_words));

    const float gap = painter.line_height(metrics.font_px);
    float current_y = 0.0f;
    bool first_paragraph = true;
    int flat = 0;
    float last_line_center = 0.0f;

    for (std::size_t p_index = 0; p_index < plan.size(); ++p_index) {
        const Paragraph& paragraph = plan[p_index];
        const float line_h = line_height_for(paragraph, metrics.font_px, painter);
        const float size = paragraph.is_direction ? layout.direction_px : metrics.font_px;
        const float pad_x = paragraph.is_direction ? 16.0f : 0.0f;
        const float pad_y = paragraph.is_direction ? 9.0f : 0.0f;
        const float max_text_width = paragraph.is_direction
            ? std::max(1.0f, layout.column_width - 2.0f * pad_x)
            : layout.column_width;

        if (first_paragraph) {
            current_y = -line_h * 0.5f;
            first_paragraph = false;
        }

        std::vector<Atom> atoms;
        for (std::size_t w = 0; w < paragraph.words.size(); ++w) {
            const Word& word = paragraph.words[w];
            const int index = flat + static_cast<int>(w);
            if (!word.text.empty()) {
                if (!atoms.empty() && !atoms.back().space) {
                    Atom space;
                    space.word = index;
                    space.char_offset = 0;
                    space.text = " ";
                    space.style = paragraph.is_direction ? FontStyle::Math
                        : (word.bold ? FontStyle::Bold : (word.italic ? FontStyle::Italic : FontStyle::Roman));
                    space.space = true;
                    space.width = painter.width(" ", size, space.style);
                    atoms.push_back(std::move(space));
                }
                const std::vector<Atom> word_atoms =
                    atoms_for_word(paragraph, word, index, painter, size, max_text_width);
                atoms.insert(atoms.end(), word_atoms.begin(), word_atoms.end());
            }
            word_slot(layout.words, index, word.is_comment, paragraph.is_direction);
            layout.word_seconds[static_cast<std::size_t>(index)] = static_cast<float>(word.seconds);
        }

        std::vector<Line> lines = wrap_atoms(atoms, max_text_width);
        if (lines.empty() && paragraph.is_direction) lines.push_back(Line{});

        float top = current_y;
        float card_width = 0.0f;
        float card_left = layout.column_left;
        if (paragraph.is_direction) {
            float widest = 0.0f;
            for (const Line& line : lines) widest = std::max(widest, line.width);
            card_width = std::min(layout.column_width, widest + 2.0f * pad_x);
            card_left = layout.column_left + (layout.column_width - card_width) * 0.5f;
        }
        const float paragraph_height = paragraph.is_direction
            ? lines.size() * line_h + 2.0f * pad_y
            : lines.size() * line_h;

        PlacedParagraph placed_para;
        placed_para.index = static_cast<int>(p_index);
        placed_para.direction = paragraph.is_direction;
        placed_para.top = top;
        placed_para.bottom = top + paragraph_height;
        placed_para.card_x = card_left;
        placed_para.card_y = top;
        placed_para.card_w = paragraph.is_direction ? card_width : 0.0f;
        placed_para.card_h = paragraph.is_direction ? paragraph_height : 0.0f;

        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const Line& line = lines[line_index];
            const float center = top + pad_y + line_h * (static_cast<float>(line_index) + 0.5f);
            last_line_center = std::max(last_line_center, center);
            float x = layout.column_left;
            if (paragraph.is_direction) {
                x = card_left + pad_x;
                const float content_width = std::max(0.0f, card_width - 2.0f * pad_x);
                if (metrics.centered && line.width < content_width) {
                    x += (content_width - line.width) * 0.5f;
                }
            } else if (metrics.centered) {
                x += (layout.column_width - line.width) * 0.5f;
            }
            const float baseline = center + size * 0.32f;
            for (const Atom& atom : line.atoms) {
                if (atom.word < 0) continue;
                const std::vector<Glyph> glyphs = painter.layout(atom.text, x, baseline, size, atom.style);
                for (Glyph glyph : glyphs) {
                    glyph.word_index = atom.word;
                    glyph.char_index = atom.char_offset + glyph.char_index;
                    glyph.line_center = center;
                    x = glyph.pen_after;
                    if (glyph.draw) {
                        PlacedWord& placed = layout.words[static_cast<std::size_t>(atom.word)];
                        add_bounds(placed, center, glyph.x0, glyph.x1);
                    }
                    layout.glyphs.push_back(std::move(glyph));
                }
            }
        }

        // Word centres and per-paragraph word records are filled from the glyph
        // stream, which is the same source the karaoke painter reads.
        for (const PlacedWord& word : layout.words) {
            if (word.index < 0) continue;
            if (!word.lines.empty()) {
                layout.word_center[static_cast<std::size_t>(word.index)] = word.first_line_center;
            }
        }
        placed_para.words = layout.words;
        layout.paragraphs.push_back(std::move(placed_para));

        current_y = top + paragraph_height + gap;
        flat += static_cast<int>(paragraph.words.size());
    }

    // Fall back to the paragraph's first line for words that drew nothing.
    for (std::size_t p_index = 0; p_index < plan.size(); ++p_index) {
        if (p_index >= layout.paragraphs.size()) break;
        const PlacedParagraph& placed = layout.paragraphs[p_index];
        const Paragraph& source = plan[p_index];
        const float fallback = placed.direction ? placed.top + placed.card_h * 0.5f
                                                : placed.top + line_height_for(source, metrics.font_px, painter) * 0.5f;
        int base = 0;
        for (std::size_t q = 0; q < p_index; ++q) base += static_cast<int>(plan[q].words.size());
        for (std::size_t w = 0; w < source.words.size(); ++w) {
            const int index = base + static_cast<int>(w);
            if (layout.word_center[static_cast<std::size_t>(index)] == 0.0f) {
                layout.word_center[static_cast<std::size_t>(index)] = fallback;
            }
        }
    }

    layout.max_scroll = std::max(0.0f, last_line_center);
    return layout;
}

} // namespace tp::gui
