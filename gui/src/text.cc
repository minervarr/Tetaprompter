#include "text.hh"

#include "fonts.hh"

#include <algorithm>
#include <cmath>
#include <memory>

#include "host.hh"
#include "raster_font.hh"
#include "renderer.hh"
#include "teleprompter/script.hh"
#include "utf8.hh"

namespace tp::gui {

::Color to_color(Rgb c, float alpha) {
    return ::Color{static_cast<float>(c.r) / 255.0f,
                   static_cast<float>(c.g) / 255.0f,
                   static_cast<float>(c.b) / 255.0f,
                   alpha};
}

TextPainter::~TextPainter() = default;

namespace {
std::string data_path(const Host& host, const std::string& path) {
    if (path.empty() || path.front() == '/') return path;
    return host.exeDir() + path;
}
} // namespace

bool TextPainter::load(Host& host, const FontPreset& preset, std::string& error) {
    // Every printable ASCII codepoint is cheap and makes the UI independent of
    // which script happens to be open this second.
    for (uint32_t cp = 32; cp < 127; ++cp) codepoints_.insert(cp);
    codepoints_.insert(0x2014); // em dash
    codepoints_.insert(0x2026); // ellipsis
    codepoints_.insert(0x00B7); // middot
    for (uint32_t cp = 0x2190; cp <= 0x2193; ++cp) codepoints_.insert(cp);
    return reload(host, preset, error);
}

bool TextPainter::open_faces(Host& host, const FontPreset& preset, std::string& error) {
    auto next = std::make_unique<RasterFont>();
    const std::string regular = data_path(host, preset.regular);
    if (regular.empty() || !next->open(host.dataReader(), regular.c_str())) {
        error = "cannot open " + (regular.empty() ? std::string("regular font") : regular);
        return false;
    }
    const std::string bold = data_path(host, preset.bold);
    const std::string italic = data_path(host, preset.italic);
    if (!bold.empty()) next->addStyle(host.dataReader(), bold.c_str(), FontStyle::Bold);
    if (!italic.empty()) next->addStyle(host.dataReader(), italic.c_str(), FontStyle::Italic);

    // Missing codepoints (OpenDyslexic has no em dash, ellipsis or arrows) go
    // to the fallback face for the matching style.
    const std::string fallback_regular = data_path(host, preset.fallback_regular);
    const std::string fallback_bold = data_path(host, preset.fallback_bold);
    const std::string fallback_italic = data_path(host, preset.fallback_italic);
    if (!fallback_regular.empty()) {
        next->addFallback(host.dataReader(), fallback_regular.c_str(), FontStyle::Roman);
    }
    if (!fallback_bold.empty()) {
        next->addFallback(host.dataReader(), fallback_bold.c_str(), FontStyle::Bold);
    }
    if (!fallback_italic.empty()) {
        next->addFallback(host.dataReader(), fallback_italic.c_str(), FontStyle::Italic);
    }

    font_ = std::move(next);
    valid_ = true;
    return true;
}

bool TextPainter::reload(Host& host, const FontPreset& preset, std::string& error) {
    return open_faces(host, preset, error);
}

bool TextPainter::collect(std::string_view text) {
    bool added = false;
    std::size_t i = 0;
    while (i < text.size()) {
        const uint32_t cp = utf8::nextCodepoint(text, i);
        if (cp != 0 && codepoints_.insert(cp).second) added = true;
    }
    return added;
}

int TextPainter::ensure(const std::vector<int>& sizes) {
    if (!valid_ || font_ == nullptr) return 0;
    for (int size : sizes) {
        if (size > 0) sizes_.insert(size);
    }
    const std::vector<uint32_t> codepoints(codepoints_.begin(), codepoints_.end());
    const std::vector<int> size_list(sizes_.begin(), sizes_.end());
    const int added = font_->ensureGlyphs(codepoints, size_list);
    return added;
}

bool TextPainter::has_misses() const {
    return valid_ && font_ != nullptr && font_->hasMisses();
}

int TextPainter::bake_misses() {
    return valid_ && font_ != nullptr ? font_->bakeMisses() : 0;
}

float TextPainter::width(std::string_view text, float size, FontStyle style) const {
    if (!valid_ || font_ == nullptr || text.empty()) return 0.0f;
    float pen = 0.0f;
    uint32_t previous = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const uint32_t cp = utf8::nextCodepoint(text, i);
        pen += font_->kernEmStyled(style, previous, cp) * size;
        if (style == FontStyle::Roman) {
            pen += font_->advance(cp, size);
        } else {
            const uint32_t key = font_->keyForStyle(style, cp);
            pen += key != 0 ? font_->advanceKey(key, size) : font_->advance(cp, size);
        }
        previous = cp;
    }
    return pen;
}

float TextPainter::line_height(float size) const {
    const float css_box = size * 1.35f;
    if (valid_ && font_ != nullptr) {
        return std::max(css_box, font_->lineHeight(size));
    }
    return css_box;
}

float TextPainter::ascender(float size) const {
    return valid_ && font_ != nullptr ? font_->ascender(size) : size * 0.8f;
}

std::vector<Glyph> TextPainter::layout(std::string_view text, float pen_x, float baseline_y,
                                       float size, FontStyle style) const {
    std::vector<Glyph> glyphs;
    if (!valid_ || font_ == nullptr || text.empty()) return glyphs;
    glyphs.reserve(text.size());
    uint32_t previous = 0;
    std::size_t i = 0;
    int char_index = 0;
    while (i < text.size()) {
        const uint32_t cp = utf8::nextCodepoint(text, i);
        GlyphQuad quad;
        if (style == FontStyle::Roman) {
            pen_x = font_->layout(cp, pen_x, baseline_y, size, quad, previous);
        } else {
            pen_x += font_->kernEmStyled(style, previous, cp) * size;
            const uint32_t key = font_->keyForStyle(style, cp);
            pen_x = key != 0 ? font_->layoutByKey(key, pen_x, baseline_y, size, quad)
                             : font_->layout(cp, pen_x, baseline_y, size, quad, 0);
        }
        Glyph glyph;
        glyph.x0 = quad.x0;
        glyph.y0 = quad.y0;
        glyph.x1 = quad.x1;
        glyph.y1 = quad.y1;
        glyph.u0 = quad.u0;
        glyph.v0 = quad.v0;
        glyph.u1 = quad.u1;
        glyph.v1 = quad.v1;
        glyph.page = quad.page;
        glyph.draw = quad.draw;
        glyph.char_index = char_index++;
        glyph.codepoint = cp;
        glyph.pen_after = pen_x;
        glyphs.push_back(glyph);
        previous = cp;
    }
    return glyphs;
}

namespace {

void emit_quad(std::vector<float>& out, const Glyph& glyph, ::Color c,
               float x0, float y0, float x1, float y1,
               float u0, float v0, float u1, float v1) {
    const float p = static_cast<float>(glyph.page);
    const float verts[6][9] = {
        {x0, y0, u0, v0, c.r, c.g, c.b, c.a, p},
        {x1, y0, u1, v0, c.r, c.g, c.b, c.a, p},
        {x1, y1, u1, v1, c.r, c.g, c.b, c.a, p},
        {x0, y0, u0, v0, c.r, c.g, c.b, c.a, p},
        {x1, y1, u1, v1, c.r, c.g, c.b, c.a, p},
        {x0, y1, u0, v1, c.r, c.g, c.b, c.a, p},
    };
    out.insert(out.end(), &verts[0][0],
               &verts[0][0] + sizeof(verts) / sizeof(verts[0][0]));
}

bool clip_glyph(const ClipRect& clip, Glyph& glyph) {
    if (!glyph.draw) return false;
    if (!clip.active) return true;
    if (glyph.x1 <= clip.x0 || glyph.x0 >= clip.x1 ||
        glyph.y1 <= clip.y0 || glyph.y0 >= clip.y1) {
        return false;
    }
    const float nx0 = std::max(glyph.x0, clip.x0);
    const float nx1 = std::min(glyph.x1, clip.x1);
    const float ny0 = std::max(glyph.y0, clip.y0);
    const float ny1 = std::min(glyph.y1, clip.y1);
    if (nx1 <= nx0 || ny1 <= ny0) return false;
    const float w = glyph.x1 - glyph.x0;
    const float h = glyph.y1 - glyph.y0;
    const float fu0 = (nx0 - glyph.x0) / w;
    const float fu1 = (nx1 - glyph.x0) / w;
    const float fv0 = (ny0 - glyph.y0) / h;
    const float fv1 = (ny1 - glyph.y0) / h;
    const float u0 = glyph.u0 + fu0 * (glyph.u1 - glyph.u0);
    const float u1 = glyph.u0 + fu1 * (glyph.u1 - glyph.u0);
    const float v0 = glyph.v0 + fv0 * (glyph.v1 - glyph.v0);
    const float v1 = glyph.v0 + fv1 * (glyph.v1 - glyph.v0);
    glyph.x0 = nx0; glyph.y0 = ny0; glyph.x1 = nx1; glyph.y1 = ny1;
    glyph.u0 = u0; glyph.v0 = v0; glyph.u1 = u1; glyph.v1 = v1;
    return true;
}

} // namespace

void TextPainter::append(std::vector<float>& out, const std::vector<Glyph>& glyphs,
                         ::Color colour, const ClipRect& clip) const {
    for (const Glyph& source : glyphs) {
        if (!source.draw) continue;
        Glyph glyph = source;
        if (!clip_glyph(clip, glyph)) continue;
        emit_quad(out, glyph, colour, glyph.x0, glyph.y0, glyph.x1, glyph.y1,
                  glyph.u0, glyph.v0, glyph.u1, glyph.v1);
    }
}

void TextPainter::append(std::vector<float>& out, const std::vector<Glyph>& glyphs,
                         const std::vector<::Color>& colours, const ClipRect& clip) const {
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const Glyph& source = glyphs[i];
        if (!source.draw) continue;
        Glyph glyph = source;
        if (!clip_glyph(clip, glyph)) continue;
        const ::Color colour = i < colours.size() ? colours[i] : ::Color{1, 1, 1, 1};
        emit_quad(out, glyph, colour, glyph.x0, glyph.y0, glyph.x1, glyph.y1,
                  glyph.u0, glyph.v0, glyph.u1, glyph.v1);
    }
}

void TextPainter::append_rect(std::vector<float>& out, float x, float y, float w, float h,
                              ::Color colour) const {
    if (!valid_ || font_ == nullptr) return;
    float u = 0.0f, v = 0.0f;
    if (!font_->solidTexel(u, v)) return;
    const float p = 0.0f;
    const float verts[6][9] = {
        {x, y, u, v, colour.r, colour.g, colour.b, colour.a, p},
        {x + w, y, u, v, colour.r, colour.g, colour.b, colour.a, p},
        {x + w, y + h, u, v, colour.r, colour.g, colour.b, colour.a, p},
        {x, y, u, v, colour.r, colour.g, colour.b, colour.a, p},
        {x + w, y + h, u, v, colour.r, colour.g, colour.b, colour.a, p},
        {x, y + h, u, v, colour.r, colour.g, colour.b, colour.a, p},
    };
    out.insert(out.end(), &verts[0][0],
               &verts[0][0] + sizeof(verts) / sizeof(verts[0][0]));
}

} // namespace tp::gui
