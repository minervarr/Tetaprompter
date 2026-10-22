#include "teleprompter/settings.hh"

#include "teleprompter/recents.hh"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace tp {
namespace {

namespace fs = std::filesystem;

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

} // namespace

std::string settings_path() {
    return (fs::path(state_dir()) / "settings.json").generic_string();
}

std::string load_font_setting() {
    std::ifstream in(settings_path(), std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    const std::string key = "\"font\"";
    std::size_t at = document.find(key);
    if (at == std::string::npos) return {};
    at = document.find(':', at + key.size());
    if (at == std::string::npos) return {};
    at = document.find('"', at + 1);
    if (at == std::string::npos) return {};
    std::size_t end = at + 1;
    while (end < document.size()) {
        if (document[end] == '\\') {
            end += 2;
            continue;
        }
        if (document[end] == '"') break;
        ++end;
    }
    if (end >= document.size()) return {};
    std::string value = document.substr(at + 1, end - at - 1);
    std::string unescaped;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            if (next == '"' || next == '\\' || next == '/') unescaped.push_back(next);
            else if (next == 'n') unescaped.push_back('\n');
            else if (next == 'r') unescaped.push_back('\r');
            else if (next == 't') unescaped.push_back('\t');
            else unescaped.push_back(next);
        } else {
            unescaped.push_back(value[i]);
        }
    }
    return unescaped;
}

bool save_font_setting(std::string_view font_id) {
    std::error_code ec;
    const fs::path target(settings_path());
    fs::create_directories(target.parent_path(), ec);
    if (ec) return false;
    std::ostringstream payload;
    payload << "{\n  \"version\": 1,\n  \"font\": \"" << json_escape(font_id) << "\"\n}\n";
    const fs::path temp = target.parent_path() /
        ("." + target.filename().string() + "." + std::to_string(::getpid()) + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string text = payload.str();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace tp
