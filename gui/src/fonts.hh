#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tp::gui {

// One selectable typeface. Paths are relative to the executable's data root
// (or absolute for a --font path supplied by the user). `fallback_*` faces are
// consulted only for codepoints the primary face does not cover, which matters
// for OpenDyslexic: it has no em dash, ellipsis or arrow glyphs.
struct FontPreset {
    std::string id;
    std::string label;
    std::string regular;
    std::string bold;
    std::string italic;
    std::string fallback_regular;
    std::string fallback_bold;
    std::string fallback_italic;
    float size_scale = 1.0f;
};

const std::vector<FontPreset>& font_presets();

// Match an id, alias, or an existing file/directory path. A directory is
// expected to contain *Regular*, *Bold* and *Italic* faces; a file is used as
// the regular face. Returns false with a diagnostic when nothing usable is
// found.
bool make_font_preset(std::string_view choice, FontPreset& out, std::string& error);

const FontPreset* find_font_preset(std::string_view id);

} // namespace tp::gui
