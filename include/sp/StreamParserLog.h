/*
 * Leid 2026.7
 *
 * StreamParserLog: pluggable logging facility for the StreamParser library.
 *
 * The library never writes to std::cout / std::cerr directly anymore. All of
 * its diagnostics go through this tiny, header-only sink so that an embedding
 * application can forward them to its own logging backend (spdlog, glog,
 * log4cxx, a custom file logger, ...).
 *
 * Typical usage from the host application:
 *
 *     #include <sp/StreamParserLog.h>
 *
 *     splog::set_callback([](const splog::Record& r) {
 *         my_logger_write(splog::level_name(r.level), r.component, r.message);
 *     });
 *     splog::set_level(splog::Level::debug);   // optional verbosity filter
 *
 * Behavior notes:
 *   - The callback is invoked on whatever thread produced the event (usually
 *     the asio io_context thread), so implementations must be thread-safe.
 *   - The callback must not block for long: it sits on the I/O path.
 *   - Exceptions thrown by the callback are swallowed by write().
 *   - When no callback is installed, records are printed to std::cerr in the
 *     historical "TAG: message" style (disable with set_stderr_fallback(false)).
 *   - The callback can be installed/replaced/cleared at any time.
 */
#pragma once
#ifndef _STREAM_PARSER_LOG_H_
#define _STREAM_PARSER_LOG_H_

#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace splog {

// Severity of a log record, ordered from most to least verbose.
enum class Level : int {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
    off   = 5   // use as minimum level to silence the library
};

// Short, upper-case name of a level ("TRACE".."ERROR"). Never returns null.
inline const char* level_name(Level level) {
    switch (level) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO";
        case Level::warn:  return "WARN";
        case Level::error: return "ERROR";
        default:           return "OFF";
    }
}

// One log event produced by the library.
struct Record {
    Level level;              // severity
    const char* component;    // "serial" / "tcp" / "tcp-client" / "tcp-server" / "udp"
    std::string message;      // human readable text, already formatted
    const char* file;         // source file that emitted the record
    int line;                 // source line that emitted the record
};

// Callback type installed by the host application.
using Callback = std::function<void(const Record&)>;

// Convenience formatting, e.g. "[WARN] [udp] receive error: Connection reset".
inline std::string format(const Record& record) {
    std::string text;
    text.reserve(48 + record.message.size());
    text += '[';
    text += level_name(record.level);
    text += "] [";
    text += (record.component ? record.component : "streamparser");
    text += "] ";
    text += record.message;
    return text;
}

namespace detail {

struct State {
    std::mutex mutex;                                    // guards callback
    Callback callback;                                   // host sink, may be empty
    std::atomic<int> min_level{static_cast<int>(Level::info)};
    std::atomic<bool> stderr_fallback{true};             // legacy console output
};

// Header-only process wide singleton (C++14 safe: inline function + local static).
inline State& state() {
    static State instance;
    return instance;
}

}  // namespace detail

// Install (or replace) the log sink. Pass an empty Callback to clear it.
inline void set_callback(Callback callback) {
    auto& state = detail::state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callback = std::move(callback);
}

// Remove the installed callback (stderr fallback, if enabled, takes over again).
inline void clear_callback() {
    set_callback(Callback());
}

// Copy of the currently installed callback (empty if none).
inline Callback callback() {
    auto& state = detail::state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.callback;
}

// Minimum severity that is forwarded. Level::off silences the library.
inline void set_level(Level level) {
    detail::state().min_level.store(static_cast<int>(level));
}

inline Level level() {
    return static_cast<Level>(detail::state().min_level.load());
}

// Keep printing to std::cerr when no callback is installed (default: on,
// preserves the historical behavior of the library).
inline void set_stderr_fallback(bool enabled) {
    detail::state().stderr_fallback.store(enabled);
}

inline bool enabled(Level level) {
    if (level == Level::off) {
        return false;
    }
    return static_cast<int>(level) >= detail::state().min_level.load();
}

// Emit one record. Called through the SP_LOG_* macros below.
inline void write(Level level, const char* component, const char* file, int line,
                  std::string message) {
    if (!enabled(level)) {
        return;
    }

    auto& state = detail::state();
    Callback sink;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        sink = state.callback;
    }

    const Record record{level, component, std::move(message), file, line};

    if (sink) {
        // A misbehaving sink must never break the parser / the I/O loop.
        try {
            sink(record);
        } catch (...) {
        }
        return;
    }

    if (state.stderr_fallback.load()) {
        std::cerr << format(record) << std::endl;
    }
}

}  // namespace splog

// -------------------------------------------------
// Emission macros
//
// The message is built with operator<< on a temporary stream, so it is only
// evaluated when the level is actually enabled:
//
//     SP_LOG_WARN("udp", "bind failed: " << ec.message());
// -------------------------------------------------
#define SP_LOG(level, component, expr)                                          \
    do {                                                                        \
        if (::splog::enabled(level)) {                                          \
            std::ostringstream splog_stream_;                                    \
            splog_stream_ << expr;                                              \
            ::splog::write(level, component, __FILE__, __LINE__,                \
                           splog_stream_.str());                                \
        }                                                                       \
    } while (0)

#define SP_LOG_TRACE(component, expr) SP_LOG(::splog::Level::trace, component, expr)
#define SP_LOG_DEBUG(component, expr) SP_LOG(::splog::Level::debug, component, expr)
#define SP_LOG_INFO(component, expr)  SP_LOG(::splog::Level::info, component, expr)
#define SP_LOG_WARN(component, expr)  SP_LOG(::splog::Level::warn, component, expr)
#define SP_LOG_ERROR(component, expr) SP_LOG(::splog::Level::error, component, expr)

#endif  // !_STREAM_PARSER_LOG_H_
