#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace glue {
inline bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
inline bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << data; return true;
}
inline long long fileStamp(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}
} // namespace glue
