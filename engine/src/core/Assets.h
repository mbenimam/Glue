#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>

namespace prism {
// Finds the assets/ folder: $PRISM_ASSETS, else the nearest "assets" going up from the
// working directory (so running from bin/Release/shaderlab or from the repo root both work).
inline std::string assetsDir() {
    namespace fs = std::filesystem;
    if (const char* e = std::getenv("PRISM_ASSETS")) return e;
    fs::path p = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(p / "assets" / "shaders")) return (p / "assets").string();
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return "assets";
}
} // namespace prism
