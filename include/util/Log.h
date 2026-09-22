//------------------------------------------------------------------------------
// Log.h
// Logging functions for the LSP server.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace server::logging {

struct Milliseconds {
    template<typename Rep, typename Period>
    explicit Milliseconds(std::chrono::duration<Rep, Period> duration) :
        value(std::chrono::round<std::chrono::milliseconds>(duration).count()) {}

    std::chrono::milliseconds::rep value;
};

/// How much of the log to write out. A message is written when its level is at or below the
/// configured one, so `debug` keeps everything and `error` only keeps failures. The per-request
/// detail that a client generates while typing is at `debug`; what the server does on its own
/// (starting up, indexing, reloading) is at `info`; anything that went wrong is at `warn` or
/// `error`.
enum class Level { off, error, warn, info, debug };

} // namespace server::logging

template<>
struct fmt::formatter<server::logging::Milliseconds> {
    constexpr auto parse(format_parse_context& context) { return context.begin(); }

    template<typename FormatContext>
    auto format(server::logging::Milliseconds duration, FormatContext& context) const {
        return fmt::format_to(context.out(), "{} ms", duration.value);
    }
};

namespace server::logging {

namespace detail {
inline thread_local FILE* output = stderr;

/// `info` by default: per-request detail is what makes a log unreadable, so it has to be asked for
inline std::atomic<int> level{static_cast<int>(Level::info)};

/// Set when the command line picked a level, which config files must not override
inline bool levelIsFixed = false;

/// Wall clock prefix for every line, so a log can be lined up with whatever the client recorded
inline std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    return fmt::format("[{:%H:%M:%S}.{:03}]", localTime, milliseconds.count());
}

template<typename... Args>
void write(std::string_view context, std::string_view prefix, fmt::format_string<Args...> format,
           Args&&... args) {
    fmt::memory_buffer buffer;
    fmt::format_to(std::back_inserter(buffer), "{} ", timestamp());
    if (!context.empty()) {
        buffer.append(context.data(), context.data() + context.size());
        buffer.push_back(' ');
    }
    buffer.append(prefix.data(), prefix.data() + prefix.size());
    fmt::format_to(std::back_inserter(buffer), format, std::forward<Args>(args)...);
    buffer.push_back('\n');
    std::fwrite(buffer.data(), 1, buffer.size(), output);
}
} // namespace detail

inline Level getLevel() {
    return static_cast<Level>(detail::level.load(std::memory_order_relaxed));
}

inline void setLevel(Level newLevel) {
    detail::level.store(static_cast<int>(newLevel), std::memory_order_relaxed);
}

/// Set the level from the command line; config files are read later and must not undo it.
inline void fixLevel(Level newLevel) {
    setLevel(newLevel);
    detail::levelIsFixed = true;
}

inline bool isLevelFixed() {
    return detail::levelIsFixed;
}

inline bool isEnabled(Level messageLevel) {
    return detail::level.load(std::memory_order_relaxed) >= static_cast<int>(messageLevel);
}

inline std::string_view toString(Level level) {
    switch (level) {
        case Level::off:
            return "off";
        case Level::error:
            return "error";
        case Level::warn:
            return "warn";
        case Level::info:
            return "info";
        case Level::debug:
            return "debug";
    }
    return "info";
}

inline std::optional<Level> parseLevel(std::string_view name) {
    for (auto level : {Level::off, Level::error, Level::warn, Level::info, Level::debug}) {
        auto text = toString(level);
        if (name.size() != text.size())
            continue;
        auto matches = std::ranges::equal(name, text, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
        if (matches)
            return level;
    }
    return std::nullopt;
}

inline FILE* setOutput(FILE* output) {
    return std::exchange(detail::output, output);
}

inline FILE* getOutput() {
    return detail::output;
}

/// Blank lines separate the batches of per-request logging, so they are only worth printing when
/// that logging is on.
inline void blankLine() {
    if (isEnabled(Level::debug))
        std::fputc('\n', detail::output);
}

template<typename... Args>
void debug(fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::debug))
        detail::write({}, "DEBUG: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void debugWithContext(std::string_view context, fmt::format_string<Args...> format,
                      Args&&... args) {
    if (isEnabled(Level::debug))
        detail::write(context, "DEBUG: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void info(fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::info))
        detail::write({}, "INFO: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void infoWithContext(std::string_view context, fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::info))
        detail::write(context, "INFO: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void warn(fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::warn))
        detail::write({}, "WARN: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void warnWithContext(std::string_view context, fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::warn))
        detail::write(context, "WARN: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void error(fmt::format_string<Args...> format, Args&&... args) {
    if (isEnabled(Level::error))
        detail::write({}, "ERROR: ", format, std::forward<Args>(args)...);
}

template<typename... Args>
void errorWithContext(std::string_view context, fmt::format_string<Args...> format,
                      Args&&... args) {
    if (isEnabled(Level::error))
        detail::write(context, "ERROR: ", format, std::forward<Args>(args)...);
}

} // namespace server::logging
