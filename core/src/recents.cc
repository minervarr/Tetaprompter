#include "teleprompter/recents.hh"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include <unistd.h>

namespace tp {
namespace {

namespace fs = std::filesystem;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

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

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string json_unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c != '\\' || i + 1 >= value.size()) {
            out.push_back(c);
            continue;
        }
        const char next = value[++i];
        switch (next) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            if (i + 4 >= value.size()) break;
            unsigned codepoint = 0;
            for (int k = 0; k < 4; ++k) {
                const char h = value[i + 1 + static_cast<std::size_t>(k)];
                codepoint <<= 4;
                if (h >= '0' && h <= '9') codepoint |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') codepoint |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') codepoint |= static_cast<unsigned>(h - 'A' + 10);
            }
            i += 4;
            append_utf8(out, codepoint);
            break;
        }
        default: out.push_back(next); break;
        }
    }
    return out;
}

bool extract_string(std::string_view object, std::string_view key, std::string& value) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t at = object.find(needle);
    if (at == std::string_view::npos) return false;
    at = object.find(':', at + needle.size());
    if (at == std::string_view::npos) return false;
    at = object.find('"', at + 1);
    if (at == std::string_view::npos) return false;
    std::size_t end = at + 1;
    while (end < object.size()) {
        if (object[end] == '\\') {
            end += 2;
            continue;
        }
        if (object[end] == '"') break;
        ++end;
    }
    if (end >= object.size()) return false;
    value = json_unescape(object.substr(at + 1, end - at - 1));
    return true;
}

bool extract_number(std::string_view object, std::string_view key, double& value) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t at = object.find(needle);
    if (at == std::string_view::npos) return false;
    at = object.find(':', at + needle.size());
    if (at == std::string_view::npos) return false;
    ++at;
    while (at < object.size() && std::isspace(static_cast<unsigned char>(object[at]))) ++at;
    const std::size_t begin = at;
    while (at < object.size()) {
        const char c = object[at];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
              c == '.' || c == 'e' || c == 'E')) {
            break;
        }
        ++at;
    }
    if (begin == at) return false;
    try {
        value = std::stod(std::string(object.substr(begin, at - begin)));
        return true;
    } catch (...) {
        return false;
    }
}

std::string_view array_region(std::string_view document) {
    std::size_t key = document.find("\"scripts\"");
    std::size_t begin = key == std::string_view::npos ? document.find('[')
                                                      : document.find('[', key);
    if (begin == std::string_view::npos) return document;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t i = begin; i < document.size(); ++i) {
        const char c = document[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '[') ++depth;
        else if (c == ']') {
            --depth;
            if (depth == 0) return document.substr(begin, i - begin + 1);
        }
    }
    return document.substr(begin);
}

std::vector<std::string_view> objects_in(std::string_view document) {
    std::vector<std::string_view> objects;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t begin = std::string_view::npos;
    for (std::size_t i = 0; i < document.size(); ++i) {
        const char c = document[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) begin = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && begin != std::string_view::npos) {
                    objects.push_back(document.substr(begin, i - begin + 1));
                }
            }
        }
    }
    return objects;
}

std::string expand_home(std::string path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home != nullptr) {
            path = std::string(home) + path.substr(1);
        }
    }
    return path;
}

} // namespace

std::string state_dir() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    fs::path base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = fs::path(xdg);
    } else {
        const char* home = std::getenv("HOME");
        base = home != nullptr ? fs::path(home) / ".local" / "state" : fs::path(".");
    }
    return (base / "teleprompter").generic_string();
}

std::string recents_path() {
    return (fs::path(state_dir()) / "recents.json").generic_string();
}

std::string resolve_path(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::path(expand_home(path));
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) {
        canonical = fs::absolute(p, ec);
        if (ec) return expand_home(path);
    }
    return canonical.generic_string();
}

std::string path_key(const std::string& path) {
    return resolve_path(path);
}

std::vector<Recent> load_recents(const std::string& path) {
    const std::string file = path.empty() ? recents_path() : path;
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();

    std::vector<Recent> entries;
    std::set<std::string> seen;
    const double now = now_seconds();
    for (std::string_view object : objects_in(array_region(document))) {
        std::string raw_path;
        double opened = 0.0;
        if (!extract_string(object, "path", raw_path) || raw_path.empty()) continue;
        if (!extract_number(object, "opened", opened)) opened = 0.0;
        if (!(opened >= 0.0 && opened <= now + 86400.0)) opened = 0.0;
        const std::string key = path_key(raw_path);
        if (seen.count(key) != 0) continue;
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(key), ec)) continue;
        seen.insert(key);
        entries.push_back(Recent{key, opened});
    }
    std::sort(entries.begin(), entries.end(), [](const Recent& a, const Recent& b) {
        return a.opened > b.opened;
    });
    if (entries.size() > static_cast<std::size_t>(kRecentsMax)) {
        entries.resize(static_cast<std::size_t>(kRecentsMax));
    }
    return entries;
}

bool save_recents(const std::vector<Recent>& entries, const std::string& path) {
    const std::string file = path.empty() ? recents_path() : path;
    std::error_code ec;
    const fs::path parent = fs::path(file).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    if (ec) return false;

    std::ostringstream payload;
    payload << "{\n  \"version\": 1,\n  \"scripts\": [\n";
    // Epoch seconds need more than the default six significant digits.
    payload << std::setprecision(17);
    const std::size_t count = std::min(entries.size(), static_cast<std::size_t>(kRecentsMax));
    for (std::size_t i = 0; i < count; ++i) {
        payload << "    {\"path\": \"" << json_escape(entries[i].path)
                << "\", \"opened\": " << entries[i].opened << "}";
        if (i + 1 != count) payload << ',';
        payload << '\n';
    }
    payload << "  ]\n}\n";

    const fs::path target(file);
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

std::vector<Recent> remember_recent(std::vector<Recent> entries,
                                    const std::string& script_path, double now) {
    const std::string key = path_key(script_path);
    if (now < 0.0) now = now_seconds();
    std::vector<Recent> fresh;
    fresh.reserve(entries.size() + 1);
    fresh.push_back(Recent{key, now});
    for (const Recent& entry : entries) {
        if (entry.path != key) fresh.push_back(entry);
    }
    if (fresh.size() > static_cast<std::size_t>(kRecentsMax)) {
        fresh.resize(static_cast<std::size_t>(kRecentsMax));
    }
    return fresh;
}

std::vector<Recent> prune_missing(std::vector<Recent> entries) {
    std::vector<Recent> out;
    out.reserve(entries.size());
    std::set<std::string> seen;
    for (const Recent& entry : entries) {
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(entry.path), ec)) continue;
        if (!seen.insert(entry.path).second) continue;
        out.push_back(entry);
        if (out.size() >= static_cast<std::size_t>(kRecentsMax)) break;
    }
    return out;
}

} // namespace tp
