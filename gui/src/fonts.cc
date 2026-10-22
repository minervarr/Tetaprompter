#include "fonts.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace tp::gui {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string absolute_path(const fs::path& path) {
    std::error_code ec;
    fs::path full = fs::weakly_canonical(path, ec);
    if (ec) full = fs::absolute(path, ec);
    if (ec) return path.generic_string();
    return full.generic_string();
}

bool font_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".otf" || ext == ".ttf";
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    return lower_ascii(std::string(haystack)).find(lower_ascii(std::string(needle))) !=
           std::string::npos;
}

// Pick the face whose filename best matches `style`. Directories supplied by a
// user are not required to use the project's exact filenames.
fs::path find_face(const fs::path& dir, std::string_view style) {
    std::error_code ec;
    std::vector<fs::path> candidates;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (font_file(it->path())) candidates.push_back(it->path());
    }

    auto has = [](const fs::path& path, std::string_view token) {
        return contains_case_insensitive(path.filename().string(), token);
    };
    int best_score = -1;
    fs::path best;
    for (const fs::path& path : candidates) {
        int score = 0;
        if (style == "regular") {
            if (has(path, "regular")) score += 4;
            if (has(path, "bold")) score -= 2;
            if (has(path, "italic")) score -= 2;
            if (has(path, "oblique")) score -= 2;
        } else if (style == "bold") {
            if (has(path, "bold")) score += 4;
            if (has(path, "italic")) score -= 2;
        } else if (style == "italic") {
            if (has(path, "italic") || has(path, "oblique")) score += 4;
            if (has(path, "bold")) score -= 2;
        } else if (style == "bolditalic") {
            if (has(path, "bold")) score += 3;
            if (has(path, "italic") || has(path, "oblique")) score += 3;
        }
        if (score > best_score) {
            best_score = score;
            best = path;
        }
    }
    return best_score > 0 ? best : fs::path{};
}

} // namespace

const std::vector<FontPreset>& font_presets() {
    static const std::vector<FontPreset> presets = {
        {
            "newcm",
            "New Computer Modern",
            "fonts/ui/NewCMSans10-Regular.otf",
            "fonts/ui/NewCMSans10-Bold.otf",
            "fonts/ui/NewCMSans10-Oblique.otf",
            "fonts/ui/NewCMSans10-BoldOblique.otf",
            "fonts/mono/NewCMMono10-Regular.otf",
            {},
            {},
            {},
        },
        {
            "opendyslexic",
            "OpenDyslexic",
            "fonts/openDix/OpenDyslexic-Regular.otf",
            "fonts/openDix/OpenDyslexic-Bold.otf",
            "fonts/openDix/OpenDyslexic-Italic.otf",
            "fonts/openDix/OpenDyslexic-BoldItalic.otf",
            "fonts/mono/NewCMMono10-Regular.otf",
            "fonts/ui/NewCMSans10-Regular.otf",
            "fonts/ui/NewCMSans10-Bold.otf",
            "fonts/ui/NewCMSans10-Oblique.otf",
            0.70f,
        },
    };
    return presets;
}

const FontPreset* find_font_preset(std::string_view id) {
    const std::string wanted = lower_ascii(std::string(id));
    for (const FontPreset& preset : font_presets()) {
        if (lower_ascii(preset.id) == wanted || lower_ascii(preset.label) == wanted) {
            return &preset;
        }
    }
    if (wanted == "normal" || wanted == "default" || wanted == "sans" ||
        wanted == "new-computer-modern") {
        return &font_presets()[0];
    }
    if (wanted == "dyslexic" || wanted == "open-dyslexic" || wanted == "opendyslexic" ||
        wanted == "opendix" || wanted == "open-dix") {
        return &font_presets()[1];
    }
    return nullptr;
}

bool make_font_preset(std::string_view choice, FontPreset& out, std::string& error) {
    if (const FontPreset* preset = find_font_preset(choice)) {
        out = *preset;
        return true;
    }

    const fs::path requested{std::string(choice)};
    std::error_code ec;
    if (fs::is_directory(requested, ec)) {
        out = {};
        out.id = "custom:" + absolute_path(requested);
        out.label = requested.filename().empty() ? "Custom font" : requested.filename().string();
        const fs::path regular = find_face(requested, "regular");
        if (!font_file(regular)) {
            error = "no regular .otf/.ttf face in " + requested.generic_string();
            return false;
        }
        out.regular = absolute_path(regular);
        const fs::path bold = find_face(requested, "bold");
        const fs::path italic = find_face(requested, "italic");
        const fs::path bold_italic = find_face(requested, "bolditalic");
        if (font_file(bold)) out.bold = absolute_path(bold);
        if (font_file(italic)) out.italic = absolute_path(italic);
        if (font_file(bold_italic)) out.bold_italic = absolute_path(bold_italic);
        out.mono = "fonts/mono/NewCMMono10-Regular.otf";
        out.fallback_regular = "fonts/ui/NewCMSans10-Regular.otf";
        out.fallback_bold = "fonts/ui/NewCMSans10-Bold.otf";
        out.fallback_italic = "fonts/ui/NewCMSans10-Oblique.otf";
        return true;
    }

    if (font_file(requested)) {
        out = {};
        const std::string full = absolute_path(requested);
        out.id = "custom:" + full;
        out.label = requested.stem().string();
        out.regular = full;
        out.mono = "fonts/mono/NewCMMono10-Regular.otf";
        out.fallback_regular = "fonts/ui/NewCMSans10-Regular.otf";
        out.fallback_bold = "fonts/ui/NewCMSans10-Bold.otf";
        out.fallback_italic = "fonts/ui/NewCMSans10-Oblique.otf";
        return true;
    }

    error = "unknown font '" + std::string(choice) +
            "' (use newcm, opendyslexic, or a .otf/.ttf path)";
    return false;
}

} // namespace tp::gui
