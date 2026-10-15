#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

namespace glue {
// Ring of recent messages so the editor console can show what the engine said.
class Log {
public:
    static void write(const char* level, const std::string& msg) {
        std::fprintf(stderr, "[%s] %s\n", level, msg.c_str());
        std::lock_guard<std::mutex> g(mutex());
        auto& l = lines();
        l.push_back(std::string("[") + level + "] " + msg);
        if (l.size() > 400) l.erase(l.begin(), l.begin() + 100);
    }
    static void info(const std::string& m) { write("info", m); }
    static void warn(const std::string& m) { write("warn", m); }
    static void error(const std::string& m) { write("error", m); }
    static std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> g(mutex());
        return lines();
    }
private:
    static std::vector<std::string>& lines() { static std::vector<std::string> v; return v; }
    static std::mutex& mutex() { static std::mutex m; return m; }
};
} // namespace glue
