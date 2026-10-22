#pragma once

#include <string>
#include <string_view>

namespace tp {

// The tiny state file for user preferences. It is deliberately separate from
// the recents cache and is always best-effort: a missing or corrupt file means
// "use the defaults", never a failed launch.
std::string settings_path();
std::string load_font_setting();
bool save_font_setting(std::string_view font_id);

} // namespace tp
