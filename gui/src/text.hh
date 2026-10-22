#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "canvas.hh"
#include "text_font.hh"
#include "teleprompter/colour.hh"

class Host;
class Renderer;
class RasterFont;

namespace tp::gui {

struct Glyph {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uint32_t page = 0;
    bool draw = false;
    int char_index = 0;
    int word_index = -1;
    float line_center = 0.0f;
    float pen_after = 0.0f;
    uint32_t codepoint = 0;
};

struct ClipRect {
    bool active = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

::Color to_color(Rgb c, float alpha = 1.0f);

// Owns the RasterFont and the per-frame text quads. Canvas's own text API draws
// one colour per string; the teleprompter needs per-character karaoke colours,
// so it lays out through TextFont directly and emits the same vertex format.
class TextPainter {
public:
    ~TextPainter();
    bool load(Host& host, std::string& error);
    bool valid() const { return valid_; }

    // Add every codepoint in `text` to the bake set. Returns true if new ones
    // appeared, so the caller knows to ensure/upload before laying out.
    bool collect(std::string_view text);
    // Bake every collected codepoint at every size seen so far. Returns the
    // number of cells added; the caller re-runs Renderer::initMsdf() when > 0.
    int ensure(const std::vector<int>& sizes);
    bool has_misses() const;
    int bake_misses();

    float width(std::string_view text, float size, FontStyle style = FontStyle::Roman) const;
    float line_height(float size) const { return size * 1.35f; }
    float ascender(float size) const;
    const RasterFont* font() const { return font_; }

    std::vector<Glyph> layout(std::string_view text, float pen_x, float baseline_y,
                              float size, FontStyle style = FontStyle::Roman) const;

    void append(std::vector<float>& out, const std::vector<Glyph>& glyphs,
                ::Color colour, const ClipRect& clip) const;
    void append(std::vector<float>& out, const std::vector<Glyph>& glyphs,
                const std::vector<::Color>& colours, const ClipRect& clip) const;

    // A solid rectangle through the text pipeline: drawn after earlier text
    // quads, so an overlay can occlude script text on top of a translucent
    // shape-layer panel.
    void append_rect(std::vector<float>& out, float x, float y, float w, float h,
                     ::Color colour) const;

private:
    RasterFont* font_ = nullptr;
    bool valid_ = false;
    std::set<uint32_t> codepoints_;
    std::set<int> sizes_;
};

} // namespace tp::gui
