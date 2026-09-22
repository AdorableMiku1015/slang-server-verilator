//------------------------------------------------------------------------------
// RequestContext.h
// Per-request logging context for JSON-RPC messages.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "JsonRpc.h"
#include "util/Log.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace lsp {

class RequestCancelled : public std::runtime_error {
public:
    explicit RequestCancelled(std::string_view checkpoint) :
        std::runtime_error(std::string(checkpoint)) {}
};

class RequestContext {
public:
    RequestContext() = default;

    RequestContext(std::string_view method, std::optional<ID_t> rpcId,
                   bool supportsCancellation = true) :
        m_id(s_nextId.fetch_add(1, std::memory_order_relaxed)), m_method(method),
        m_rpcId(std::move(rpcId)), m_logId(makeLogId(m_id, m_rpcId)),
        m_start(std::chrono::steady_clock::now()), m_supportsCancellation(supportsCancellation),
        m_state((m_rpcId || supportsCancellation) ? std::make_shared<State>() : nullptr) {}

    explicit operator bool() const { return m_id != 0; }

    uint64_t id() const { return m_id; }
    std::string_view method() const { return m_method; }
    const std::optional<ID_t>& rpcId() const { return m_rpcId; }
    bool supportsCancellation() const { return m_supportsCancellation; }

    void cancel() const {
        if (m_state)
            m_state->cancelled.store(true, std::memory_order_relaxed);
    }

    bool isCancelled() const {
        return m_state && m_state->cancelled.load(std::memory_order_relaxed);
    }

    void markStarted() const {
        if (m_state)
            m_state->started.store(true, std::memory_order_relaxed);
    }

    bool hasStarted() const { return m_state && m_state->started.load(std::memory_order_relaxed); }

    void throwIfCancelled(std::string_view checkpoint) const {
        if (isCancelled())
            throw RequestCancelled(checkpoint);
    }

    std::string elapsedContext() const {
        const auto elapsed = std::chrono::round<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - m_start)
                                 .count();
        if (elapsed < 1000)
            return fmt::format("[{}        +.{:03}]", m_logId, elapsed);
        return fmt::format("[{} {:+12.3f}]", m_logId, elapsed / 1000.0);
    }

    /// Per-request detail: hidden unless the log level is `debug`, since a client asks for one of
    /// these for every keystroke and they drown out anything worth reading.
    template<typename... Args>
    void debug(fmt::format_string<Args...> format, Args&&... args) const {
        if (!server::logging::isEnabled(server::logging::Level::debug))
            return;
        if (*this)
            server::logging::debugWithContext(elapsedContext(), format,
                                              std::forward<Args>(args)...);
        else
            server::logging::debug(format, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> format, Args&&... args) const {
        if (!*this) {
            server::logging::info(format, std::forward<Args>(args)...);
            return;
        }
        if (server::logging::isEnabled(server::logging::Level::info))
            server::logging::infoWithContext(elapsedContext(), format, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::errorWithContext(elapsedContext(), format,
                                              std::forward<Args>(args)...);
        }
        else {
            server::logging::error(format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void warn(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::warnWithContext(elapsedContext(), format, std::forward<Args>(args)...);
        }
        else {
            server::logging::warn(format, std::forward<Args>(args)...);
        }
    }

private:
    struct State {
        std::atomic_bool cancelled = false;
        std::atomic_bool started = false;
    };

    static std::string makeLogId(uint64_t contextId, const std::optional<ID_t>& rpcId) {
        if (rpcId) {
            return std::visit([](const auto& id) { return fmt::format("#{}", id); }, *rpcId);
        }
        return fmt::format("n{}", contextId);
    }

    inline static std::atomic<uint64_t> s_nextId = 1;

    uint64_t m_id = 0;
    std::string m_method;
    std::optional<ID_t> m_rpcId;
    std::string m_logId;
    std::chrono::steady_clock::time_point m_start;
    bool m_supportsCancellation = false;
    std::shared_ptr<State> m_state;
};

} // namespace lsp
