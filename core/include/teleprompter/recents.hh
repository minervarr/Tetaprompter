#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tp {

struct Recent {
    std::string path;
    double opened = 0.0;
};

inline constexpr int kRecentsMax = 30;

std::string state_dir();
std::string recents_path();
std::string resolve_path(const std::string& path);
std::string path_key(const std::string& path);

std::vector<Recent> load_recents(const std::string& path = "");
bool save_recents(const std::vector<Recent>& entries, const std::string& path = "");
std::vector<Recent> remember_recent(std::vector<Recent> entries,
                                    const std::string& script_path,
                                    double now = -1.0);
std::vector<Recent> prune_missing(std::vector<Recent> entries);

} // namespace tp
