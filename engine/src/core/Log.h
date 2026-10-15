#pragma once
// The engine log, built on spdlog (vendored header-only under third_party/spdlog).
// Messages go to stderr through spdlog's colour sink and into a ring of recent entries, which
// the editor console draws coloured by level.
#define SPDLOG_HEADER_ONLY  // one translation of spdlog per program; every TU sees the same define
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace glue {

// Severity, lowest first. Messages below the active level are dropped at the source.
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct LogEntry {
    LogLevel level;
    std::string text;
};

namespace detail {

// Keeps the last few hundred messages for the editor console.
class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    static std::vector<LogEntry>& ring() { static std::vector<LogEntry> v; return v; }
    static std::mutex& ringMutex() { static std::mutex m; return m; }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        LogLevel level = LogLevel::Info;
        switch (msg.level) {
            case spdlog::level::debug: case spdlog::level::trace: level = LogLevel::Debug; break;
            case spdlog::level::warn: level = LogLevel::Warn; break;
            case spdlog::level::err: case spdlog::level::critical: level = LogLevel::Error; break;
            default: break;
        }
        std::lock_guard<std::mutex> g(ringMutex());
        auto& r = ring();
        r.push_back({level, std::string(msg.payload.data(), msg.payload.size())});
        if (r.size() > 400) r.erase(r.begin(), r.begin() + 100);
    }
    void flush_() override {}
};

inline spdlog::level::level_enum toSpd(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        default: return spdlog::level::info;
    }
}

} // namespace detail

class Log {
public:
    static void write(LogLevel level, const std::string& msg) { logger().log(detail::toSpd(level), msg); }
    static void debug(const std::string& m) { logger().debug(m); }
    static void info(const std::string& m) { logger().info(m); }
    static void warn(const std::string& m) { logger().warn(m); }
    static void error(const std::string& m) { logger().error(m); }

    static void setLevel(LogLevel l) { logger().set_level(detail::toSpd(l)); }
    static LogLevel level() {
        switch (logger().level()) {
            case spdlog::level::debug: case spdlog::level::trace: return LogLevel::Debug;
            case spdlog::level::warn: return LogLevel::Warn;
            case spdlog::level::err: case spdlog::level::critical: return LogLevel::Error;
            default: return LogLevel::Info;
        }
    }
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
        std::lock_guard<std::mutex> g(detail::RingSink::ringMutex());
        return detail::RingSink::ring();
    }

private:
    static spdlog::logger& logger() {
        static std::shared_ptr<spdlog::logger> l = [] {
            auto color = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto ring = std::make_shared<detail::RingSink>();
            auto lg = std::make_shared<spdlog::logger>("glue", spdlog::sinks_init_list{color, ring});
            lg->set_pattern("%^[%l]%$ %v");   // %^...%$ colours the whole prefix by level
            const char* e = std::getenv("GLUE_LOG_LEVEL");
            lg->set_level(detail::toSpd(parse(e ? e : "info")));
            return lg;
        }();
        return *l;
    }
};

} // namespace glue
