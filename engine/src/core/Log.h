#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <unistd.h>
#include <cstdlib>

namespace glue {

// Severity, lowest first. Messages below the active level are dropped at the source.
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct LogEntry {
    LogLevel level;
    std::string text;
};

// The engine's log: printed to stderr (coloured when it is a terminal) and kept as a ring of
// recent entries so the editor console can show them, coloured by level.
// The level comes from GLUE_LOG_LEVEL (debug, info, warn, error; default info) and can be
// changed at run time with setLevel or the console's level selector.
class Log {
public:
    static void write(LogLevel level, const std::string& msg) {
        if (level < minLevel()) return;
        if (isatty(fileno(stderr))) std::fprintf(stderr, "%s[%s] %s\033[0m\n", ansi(level), name(level), msg.c_str());
        else std::fprintf(stderr, "[%s] %s\n", name(level), msg.c_str());
        std::lock_guard<std::mutex> g(mutex());
        auto& l = entries();
        l.push_back({level, msg});
        if (l.size() > 400) l.erase(l.begin(), l.begin() + 100);
    }
    static void debug(const std::string& m) { write(LogLevel::Debug, m); }
    static void info(const std::string& m) { write(LogLevel::Info, m); }
    static void warn(const std::string& m) { write(LogLevel::Warn, m); }
    static void error(const std::string& m) { write(LogLevel::Error, m); }

    static void setLevel(LogLevel l) { minLevel() = l; }
    static LogLevel level() { return minLevel(); }
    // "debug", "info", "warn" or "error" (also "warning"); anything else keeps info.
    static LogLevel parse(const std::string& s) {
        if (s == "debug") return LogLevel::Debug;
        if (s == "warn" || s == "warning") return LogLevel::Warn;
        if (s == "error") return LogLevel::Error;
        return LogLevel::Info;
    }
    static const char* name(LogLevel l) {
        switch (l) {
            case LogLevel::Debug: return "debug";
            case LogLevel::Info: return "info";
            case LogLevel::Warn: return "warn";
            default: return "error";
        }
    }
    static std::vector<LogEntry> snapshot() {
        std::lock_guard<std::mutex> g(mutex());
        return entries();
    }

private:
    static const char* ansi(LogLevel l) {
        switch (l) {
            case LogLevel::Debug: return "\033[90m";     // grey
            case LogLevel::Warn: return "\033[33m";      // yellow
            case LogLevel::Error: return "\033[31m";     // red
            default: return "";                          // info: terminal default
        }
    }
    static LogLevel& minLevel() {
        static LogLevel v = [] { const char* e = std::getenv("GLUE_LOG_LEVEL"); return parse(e ? e : "info"); }();
        return v;
    }
    static std::vector<LogEntry>& entries() { static std::vector<LogEntry> v; return v; }
    static std::mutex& mutex() { static std::mutex m; return m; }
};
} // namespace glue
