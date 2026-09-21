//------------------------------------------------------------------------------
// JsonRpcServer.h
// Template-based JSON-RPC server implementation with type-safe method registration
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once
#include "JsonRpc.h"
#include "LspTypes.h"
#include "RequestContext.h"
#include "util/Log.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lsp {

template<typename Impl>
class JsonRpcServer {
protected:
    std::unordered_map<std::string,
                       std::function<rfl::Generic(rfl::Generic, const RequestContext&)>>
        requests;
    std::unordered_map<std::string, std::function<void(rfl::Generic, const RequestContext&)>>
        notifications;

    template<typename P, typename R, auto Method>
    void registerMethod(const std::string& name) {
        constexpr bool acceptsContext =
            std::is_same_v<P, std::nullopt_t>
                ? std::is_invocable_v<decltype(Method), Impl*, std::monostate,
                                      const RequestContext&>
                : std::is_invocable_v<decltype(Method), Impl*, P&, const RequestContext&>;
        if constexpr (acceptsContext)
            cancellableMethods.emplace(name);

        requests[name] = [this](std::optional<rfl::Generic> paramsJson,
                                const RequestContext& ctx) -> rfl::Generic {
            auto getResult = [&]() -> R {
                if constexpr (!std::is_same_v<P, std::nullopt_t>) {
                    auto params = rfl::from_generic<P, rfl::UnderlyingEnums>(paramsJson.value());
                    if (!params)
                        throw std::runtime_error(params.error().what());

                    if constexpr (std::is_invocable_v<decltype(Method), Impl*, P&,
                                                      const RequestContext&>) {
                        return (static_cast<Impl*>(this)->*Method)(params.value(), ctx);
                    }
                    else {
                        return (static_cast<Impl*>(this)->*Method)(params.value());
                    }
                }
                else {
                    if constexpr (std::is_invocable_v<decltype(Method), Impl*, std::monostate,
                                                      const RequestContext&>) {
                        return (static_cast<Impl*>(this)->*Method)(std::monostate{}, ctx);
                    }
                    else {
                        return (static_cast<Impl*>(this)->*Method)(std::monostate{});
                    }
                }
            };

            if constexpr (std::is_same_v<R, std::monostate>) {
                getResult();
                return std::nullopt;
            }
            else {
                return rfl::to_generic<rfl::UnderlyingEnums>(getResult());
            }
        };
    }

    template<typename P, auto Method>
    void registerNotification(const std::string& name) {
        constexpr bool acceptsContext =
            std::is_same_v<P, std::nullopt_t>
                ? std::is_invocable_v<decltype(Method), Impl*, std::nullopt_t,
                                      const RequestContext&>
                : std::is_invocable_v<decltype(Method), Impl*, P&, const RequestContext&>;
        if constexpr (acceptsContext)
            cancellableMethods.emplace(name);

        notifications[name] = [this](std::optional<rfl::Generic> paramsJson,
                                     const RequestContext& ctx) {
            if constexpr (!std::is_same_v<P, std::nullopt_t>) {
                auto params = rfl::from_generic<P, rfl::UnderlyingEnums>(paramsJson.value());
                if (!params)
                    throw std::runtime_error(params.error().what());

                if constexpr (std::is_invocable_v<decltype(Method), Impl*, P&,
                                                  const RequestContext&>) {
                    (static_cast<Impl*>(this)->*Method)(params.value(), ctx);
                }
                else {
                    (static_cast<Impl*>(this)->*Method)(params.value());
                }
            }
            else {
                if constexpr (std::is_invocable_v<decltype(Method), Impl*, std::nullopt_t,
                                                  const RequestContext&>) {
                    (static_cast<Impl*>(this)->*Method)(std::nullopt, ctx);
                }
                else {
                    (static_cast<Impl*>(this)->*Method)(std::nullopt);
                }
            }
        };
    }

    RequestContext createContext(const RpcRequest& request) const {
        return RequestContext(request.method, request.id,
                              cancellableMethods.contains(request.method));
    }

    std::optional<std::string> getDidChangeKey(const RpcRequest& request) const {
        if (request.method != "textDocument/didChange" || !request.params)
            return std::nullopt;

        auto params = rfl::from_generic<DidChangeTextDocumentParams, rfl::UnderlyingEnums>(
            *request.params);
        if (!params)
            return std::nullopt;
        return params->textDocument.uri.str();
    }

    void registerContext(const RpcRequest& request, const RequestContext& ctx) {
        if (!request.id && !ctx.supportsCancellation())
            return;

        std::lock_guard lock(cancellationMutex);
        if (request.id)
            activeRequests.insert_or_assign(*request.id, ctx);

        if (ctx.supportsCancellation()) {
            auto key = getDidChangeKey(request);
            if (!key)
                return;

            auto it = pendingDocumentChanges.find(*key);
            if (it != pendingDocumentChanges.end() && it->second.id() != ctx.id())
                it->second.cancel();
            pendingDocumentChanges.insert_or_assign(std::move(*key), ctx);
        }
    }

    void unregisterContext(const RpcRequest& request, const RequestContext& ctx) {
        if (!request.id && !ctx.supportsCancellation())
            return;

        std::lock_guard lock(cancellationMutex);
        if (request.id) {
            auto it = activeRequests.find(*request.id);
            if (it != activeRequests.end() && it->second.id() == ctx.id())
                activeRequests.erase(it);
        }

        if (ctx.supportsCancellation()) {
            auto key = getDidChangeKey(request);
            if (!key)
                return;

            auto it = pendingDocumentChanges.find(*key);
            if (it != pendingDocumentChanges.end() && it->second.id() == ctx.id())
                pendingDocumentChanges.erase(it);
        }
    }

    void cancelRequest(ID_t rpcId) {
        {
            std::lock_guard lock(cancellationMutex);
            if (auto it = activeRequests.find(rpcId); it != activeRequests.end()) {
                auto target = it->second;
                if (!target.hasStarted() || target.supportsCancellation()) {
                    target.cancel();
                    target.info("<--- $/cancelRequest - cancelling {}", target.method());
                }
                else {
                    target.info("<--- $/cancelRequest - {} does not support cancellation",
                                target.method());
                }
                return;
            }
        }

        RequestContext cancelCtx("$/cancelRequest", std::move(rpcId), false);
        cancelCtx.info("<--- $/cancelRequest - cancel requested but already returned");
    }

    void startRequest(const RequestContext& ctx) {
        std::lock_guard lock(cancellationMutex);
        ctx.throwIfCancelled("before handler");
        ctx.markStarted();
        ctx.info("Started {}", ctx.method());
    }

    /// Hook for the implementation: called when a handler failed but the server carried on, so the
    /// user can be pointed at the log. Does nothing by default; `Impl` may define its own.
    void onInternalError(std::string_view method, std::string_view message) {
        (void)method;
        (void)message;
    }

    /// Failure was already logged by the caller; this only tells the implementation about it.
    /// Callers must hold serverStateMutex, since the notification is written to the same stream as
    /// responses.
    void notifyInternalError(std::string_view method, std::string_view message) {
        try {
            static_cast<Impl*>(this)->onInternalError(method, message);
        }
        catch (...) {
            // Reporting that something went wrong must never go wrong itself
        }
    }

    std::variant<rfl::Generic, RpcError, std::nullopt_t> processMessage(RpcRequest request,
                                                                        RequestContext ctx = {},
                                                                        bool logStart = true) {
        struct MessageLog {
            explicit MessageLog(RequestContext ctx, bool logStart) :
                ctx(std::move(ctx)), enabled(this->ctx.method() != "$/cancelRequest") {
                if (enabled && logStart)
                    this->ctx.startInfo("<--- {}", this->ctx.method());
            }

            ~MessageLog() {
                if (!enabled)
                    return;

                if (cancellationPoint) {
                    if (ctx.rpcId()) {
                        ctx.info("-/-> {} (request cancelled {})", ctx.method(),
                                 *cancellationPoint);
                    }
                    else {
                        ctx.info("---- {} (notification superseded {})", ctx.method(),
                                 *cancellationPoint);
                    }
                }
                else if (error) {
                    ctx.error("-/-> {} Error: {}", ctx.method(), *error);
                }
                else if (ctx.rpcId()) {
                    ctx.info("---> {}", ctx.method());
                }
                else {
                    ctx.info("---- {} (notification finished)", ctx.method());
                }
            }

            void setError(std::string message) { error = std::move(message); }
            void setCancelled(std::string checkpoint) { cancellationPoint = std::move(checkpoint); }

            RequestContext ctx;
            bool enabled;
            std::optional<std::string> error;
            std::optional<std::string> cancellationPoint;
        };

        if (!ctx)
            ctx = createContext(request);

        if (!request.id) {
            auto it = notifications.find(request.method);
            if (it != notifications.end()) {
                MessageLog messageLog(ctx, logStart);
                try {
                    if (request.params)
                        it->second(*request.params, messageLog.ctx);
                    else
                        it->second(std::nullopt, messageLog.ctx);

                    messageLog.ctx.throwIfCancelled("before completion");
                }
                catch (const RequestCancelled& e) {
                    messageLog.setCancelled(e.what());
                }
                catch (const std::exception& e) {
                    messageLog.setError(e.what());
                    notifyInternalError(request.method, e.what());
                }
                catch (...) {
                    messageLog.setError("unknown exception");
                    notifyInternalError(request.method, "unknown exception");
                }
            }
            else if (request.method.starts_with("$/")) {
                server::logging::warn("<-/- {} (ignoring threaded req)", request.method);
            }
            else {
                server::logging::warn("<-/- {} (method not found)", request.method);
            }
            return std::nullopt;
        }

        auto it = requests.find(request.method);
        if (it == requests.end()) {
            server::logging::warn("<-/- {} (not found)", request.method);
            return std::nullopt;
        }

        MessageLog messageLog(ctx, logStart);
        try {
            startRequest(messageLog.ctx);

            rfl::Generic response;
            if (request.params)
                response = it->second(*request.params, messageLog.ctx);
            else
                response = it->second(rfl::Generic{}, messageLog.ctx);

            messageLog.ctx.throwIfCancelled("before response");
            return response;
        }
        catch (const RequestCancelled& e) {
            messageLog.setCancelled(e.what());
            return RpcError{.code = static_cast<int>(LSPErrorCodes::RequestCancelled),
                            .message = "Request cancelled"};
        }
        catch (const std::exception& e) {
            messageLog.setError(e.what());
            notifyInternalError(request.method, e.what());
            return RpcError{.code = static_cast<int>(ErrorCodes::InternalError),
                            .message = e.what()};
        }
        catch (...) {
            messageLog.setError("unknown exception");
            notifyInternalError(request.method, "unknown exception");
            return RpcError{.code = static_cast<int>(ErrorCodes::InternalError),
                            .message = "Unknown exception"};
        }
    }

    void handleMessage(RpcRequest request) {
        auto ctx = createContext(request);
        registerContext(request, ctx);
        handleMessage(std::move(request), std::move(ctx));
    }

    void handleMessage(RpcRequest request, RequestContext ctx, bool logStart = true) {
        std::lock_guard<std::mutex> lock(serverStateMutex);
        auto result = processMessage(request, ctx, logStart);
        unregisterContext(request, ctx);
        try {
            std::visit(
                [request](auto&& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, rfl::Generic>) {
                        sendMessage(RpcResponse{
                            .jsonrpc = "2.0",
                            .id = request.id,
                            .result = value,
                        });
                    }
                    else if constexpr (std::is_same_v<T, RpcError>) {
                        sendMessage(RpcErrorResponse{
                            .jsonrpc = "2.0",
                            .id = request.id,
                            .error = value,
                        });
                    }
                },
                result);
        }
        catch (const std::exception& e) {
            // A result that can't be serialized (invalid UTF-8 from a source file, for example)
            // must not leave the client waiting for a response that will never come
            server::logging::error("Failed to serialize the response to {}: {}", request.method,
                                   e.what());
            try {
                sendMessage(RpcErrorResponse{
                    .jsonrpc = "2.0",
                    .id = request.id,
                    .error =
                        RpcError{
                            .code = static_cast<int>(ErrorCodes::InternalError),
                            .message = "Failed to serialize the response",
                        },
                });
            }
            catch (...) {
                server::logging::error("Failed to report the serialization failure to the client");
            }
        }
    }

    // Protects server state by serializing LSP and WCP handler execution.
    std::mutex serverStateMutex;

    std::unordered_set<std::string> cancellableMethods;

    // Protects activeRequests and pendingDocumentChanges.
    std::mutex cancellationMutex;

    // Tracks every active request so cancellation can distinguish unsupported routes.
    std::unordered_map<ID_t, RequestContext> activeRequests;

    // Successive /didChange requests should cancel earlier ones doing analysis
    std::unordered_map<std::string, RequestContext> pendingDocumentChanges;

public:
    void run() {
        std::string inputLine;
        std::string inputContent;

        // Init loop
        while (true) {
            auto request = readJson<RpcRequest>(inputLine, inputContent);
            if (!request)
                return;

            if (request->method != "initialize") {
                sendMessage(RpcErrorResponse{
                    .jsonrpc = "2.0",
                    .id = request->id,
                    .error =
                        RpcError{
                            .code = static_cast<int>(ErrorCodes::ServerNotInitialized),
                            .message = "Server not initialized",
                        },
                });
                continue;
            }
            handleMessage(*request);
            server::logging::blankLine();
            break;
        }

        struct QueuedMessage {
            RpcRequest request;
            RequestContext ctx;
        };
        std::deque<QueuedMessage> queue;

        // Protects the queue, worker state, and deferred log separator.
        std::mutex queueMutex;
        std::condition_variable queueCondition;
        bool inputFinished = false;
        bool workerBusy = false;
        bool separatorPending = false;

        // worker- processes messages in order
        auto* logOutput = server::logging::getOutput();
        std::thread worker([&, logOutput] {
            server::logging::setOutput(logOutput);
            while (true) {
                QueuedMessage message;
                {
                    std::unique_lock lock(queueMutex);
                    queueCondition.wait(lock, [&] { return inputFinished || !queue.empty(); });
                    if (queue.empty())
                        return;
                    message = std::move(queue.front());
                    queue.pop_front();
                    workerBusy = true;
                }
                // A handler that lets an exception escape used to abort the whole process, with no
                // trace of the reason; report it and keep serving instead
                auto method = std::string(message.request.method);
                try {
                    handleMessage(std::move(message.request), std::move(message.ctx), false);
                }
                catch (const std::exception& e) {
                    std::lock_guard lock(serverStateMutex);
                    server::logging::error("Uncaught exception while handling {}: {}", method,
                                           e.what());
                    notifyInternalError(method, e.what());
                }
                catch (...) {
                    std::lock_guard lock(serverStateMutex);
                    server::logging::error("Uncaught exception while handling {}", method);
                    notifyInternalError(method, "unknown exception");
                }
                {
                    std::lock_guard lock(queueMutex);
                    workerBusy = false;
                    if (queue.empty())
                        separatorPending = true;
                }
            }
        });

        auto printPendingSeparatorLocked = [&] {
            if (separatorPending) {
                server::logging::blankLine();
                separatorPending = false;
            }
        };

        auto enqueue = [&](RpcRequest queuedRequest) {
            auto ctx = createContext(queuedRequest);
            registerContext(queuedRequest, ctx);
            {
                std::lock_guard lock(queueMutex);
                printPendingSeparatorLocked();
                ctx.startInfo("<--- {}", ctx.method());
                queue.push_back({std::move(queuedRequest), std::move(ctx)});
            }
            queueCondition.notify_one();
        };

        auto handleCancelRequest = [&](RpcRequest cancellationRequest) {
            {
                std::lock_guard lock(queueMutex);
                printPendingSeparatorLocked();
            }
            processMessage(std::move(cancellationRequest));
            {
                std::lock_guard lock(queueMutex);
                if (!workerBusy && queue.empty())
                    separatorPending = true;
            }
        };

        // Main loop - reads stdin
        bool shutdown = false;
        int parseFailures = 0;
        while (true) {
            std::optional<RpcRequest> request;
            try {
                request = readJson<RpcRequest>(inputLine, inputContent);
            }
            catch (const std::exception& e) {
                // The message was consumed, so the stream is still framed correctly
                server::logging::error("Failed to parse an incoming message: {}", e.what());
                if (++parseFailures > 5) {
                    server::logging::error("Giving up after repeated unparseable messages");
                    break;
                }
                continue;
            }
            catch (...) {
                server::logging::error("Failed to parse an incoming message");
                if (++parseFailures > 5)
                    break;
                continue;
            }
            parseFailures = 0;

            if (!request) {
                // The client closed the connection; exiting cleanly is the expected response
                server::logging::info("Input stream closed, shutting down");
                break;
            }

            try {
                shutdown = request->method == "shutdown";
                if (request->method == "$/cancelRequest")
                    handleCancelRequest(std::move(*request));
                else
                    enqueue(std::move(*request));
            }
            catch (const std::exception& e) {
                std::lock_guard lock(serverStateMutex);
                server::logging::error("Uncaught exception while queueing a message: {}", e.what());
                notifyInternalError("incoming message", e.what());
                continue;
            }
            catch (...) {
                std::lock_guard lock(serverStateMutex);
                server::logging::error("Uncaught exception while queueing a message");
                notifyInternalError("incoming message", "unknown exception");
                continue;
            }

            if (shutdown)
                break;
        }

        // Shutdown loop
        if (shutdown) {
            // Anything that escapes here would skip the join below, and a joinable std::thread
            // destroyed during unwinding terminates the process
            try {
                while (auto request = readJson<RpcRequest>(inputLine, inputContent)) {
                    if (request->method == "exit")
                        break;

                    if (request->method == "$/cancelRequest") {
                        handleCancelRequest(std::move(*request));
                    }
                    else {
                        sendMessage(RpcErrorResponse{
                            .jsonrpc = "2.0",
                            .id = request->id,
                            .error =
                                RpcError{
                                    .code = static_cast<int>(ErrorCodes::InvalidRequest),
                                    .message = "Invalid Request",
                                },
                        });
                    }
                }
            }
            catch (const std::exception& e) {
                server::logging::error("Uncaught exception during shutdown: {}", e.what());
            }
            catch (...) {
                server::logging::error("Uncaught exception during shutdown");
            }
        }

        {
            std::lock_guard lock(queueMutex);
            inputFinished = true;
        }
        queueCondition.notify_one();
        worker.join();
    }
};
} // namespace lsp
