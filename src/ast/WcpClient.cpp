//------------------------------------------------------------------------------
/// @file WcpClient.h
/// @brief Waveform viewer Control Protocol client
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ast/WcpClient.h"

#include "fmt/format.h"
#include "rfl/UnderlyingEnums.hpp"
#include "rfl/to_generic.hpp"
#include "util/Logging.h"
#include "wcp/WcpTypes.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
// Handle Windows macro collision
#    define NOMINMAX

#    include <process.h>
#    include <windows.h>
#else
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace fs = std::filesystem;

void waves::WcpClient::runViewer() {
#ifdef _WIN32
    std::string cmdLine = fmt::format(fmt::runtime(m_command), m_port);

    auto stdoutLog = fs::temp_directory_path();
    stdoutLog /= "slang-server.wcp.stdout";
    auto stderrLog = fs::temp_directory_path();
    stderrLog /= "slang-server.wcp.stderr";

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hStdout = CreateFileA(stdoutLog.string().c_str(), GENERIC_WRITE, 0, &saAttr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hStderr = CreateFileA(stderrLog.string().c_str(), GENERIC_WRITE, 0, &saAttr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hStdout;
    si.hStdError = hStderr;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, const_cast<char*>(cmdLine.c_str()), NULL, NULL, TRUE, 0, NULL, NULL,
                        &si, &pi)) {
        ERROR("Problem launching waveform viewer: {}", GetLastError());
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdout);
    CloseHandle(hStderr);
#else
    if (fork() == 0) {
        auto stdoutLog = fs::temp_directory_path();
        stdoutLog /= "slang-server.wcp.stdout";
        auto stderrLog = fs::temp_directory_path();
        stderrLog /= "slang-server.wcp.stderr";

        if (!freopen(stdoutLog.c_str(), "w", stdout)) {
            WARN("Failed to redirect stdout to {}", stdoutLog.string());
        }
        if (!freopen(stderrLog.c_str(), "w", stderr)) {
            WARN("Failed to redirect stderr to {}", stderrLog.string());
        }

        std::vector<char*> args;
        std::stringstream ss(fmt::format(fmt::runtime(m_command), m_port));
        std::string token;

        while (ss >> token) {
            args.push_back(strdup(token.c_str()));
        }
        args.push_back(nullptr);

        // TODO -- make this persist even if slang-server goes down, maybe a switch for that
        // behavior?
        execvp(args[0], args.data());
        ERROR("Problem launching waveform viewer: {}", strerror(errno));
        exit(0);
    }
#endif
}

void waves::WcpClient::initClient() {
    // Create socket
    if ((m_serverFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Problem creating WCP socket: {}", strerror(errno));
        stop();
        return;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = 0;
    if (bind(m_serverFd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ERROR("Problem binding to WCP port: {}", strerror(errno));
        stop();
        return;
    }

    // Find port
    struct sockaddr_in assignedAddr;
    socklen_t len = sizeof(assignedAddr);
    if (getsockname(m_serverFd, (struct sockaddr*)&assignedAddr, &len) < 0) {
        ERROR("Problem discovering WCP port number: {}", strerror(errno));
        stop();
        return;
    }
    m_port = ntohs(assignedAddr.sin_port);
}

void waves::WcpClient::greet() {
    // Listen + accept connection
    if (listen(m_serverFd, 1) < 0) {
        ERROR("Problem listening to WCP port: {}", strerror(errno));
        stop();
        return;
    }

    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    if ((m_clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientAddrLen)) < 0) {
        ERROR("Problem accepting WCP connection: {}", strerror(errno));
        stop();
        return;
    }

#ifdef __linux__
    int flag = 1;
    setsockopt(m_clientFd, IPPROTO_TCP, TCP_QUICKACK, (char*)&flag, sizeof(int));
#endif

    INFO("WCP connection established");

    // Send greeting
    sendMessage(rfl::to_generic<rfl::UnderlyingEnums>(wcp::Greeting{
        .type = "greeting",
        .version = "0",
        .commands = {"waveforms_loaded", "goto_declaration", "add_drivers", "add_loads"},
    }));

    // Receive greeting
    std::optional<std::string> s2cGreeting;
    auto greetingStart = std::chrono::steady_clock::now();
    while (!(s2cGreeting = getMessage())) {
        std::chrono::duration<double> greetingWait = std::chrono::steady_clock::now() -
                                                     greetingStart;
        if (greetingWait.count() > 2.0) {
            WARN("WCP timed out waiting for server greeting");
            stop();
            return;
        }
    }

    auto greeting = rfl::json::read<wcp::Greeting>(*s2cGreeting);
    // Handle server greeting
    if (greeting) {
        if (greeting->type != "greeting") {
            ERROR("WCP greeting was not a greeting type");
            stop();
        }
        if (greeting->version != "0") {
            ERROR("WCP greeting was not version 0");
            stop();
        }

        // Check capabilities
        const std::vector<std::string> requiredCommands = {
            "add_items", "get_item_list", "get_item_info", "focus_item", "load",
        };
        for (const auto& requiredCommand : requiredCommands) {
            if (std::find(greeting->commands.begin(), greeting->commands.end(), requiredCommand) ==
                greeting->commands.end()) {
                ERROR("WCP greeting did not contain {} command", requiredCommand);
                stop();
            }
        }
    }
    else {
        ERROR("Problem decoding WCP greeting");
        stop();
    }
}

void waves::WcpClient::runClient() {

    // Handle events and responses
    while (m_running) {
        auto message = getMessage();
        if (!message) {
            continue;
        }

        std::lock_guard<std::mutex> lock(m_lspServer->getServerStateMutex());
        DEBUG("WCP S2C MESSAGE: {}", *message);
        auto s2cMessage = rfl::json::read<wcp::S2CMessage>(*message);
        if (!s2cMessage) {
            ERROR("WCP S2C decode error");
            continue;
        }

        rfl::visit(
            [&](auto&& msg) {
                using Type = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same<Type, wcp::Response>()) {
                }
                else if constexpr (std::is_same<Type, wcp::Event>()) {
                    auto s2cEvent = rfl::json::read<wcp::S2CEvent>(*message);
                    if (!s2cEvent) {
                        ERROR("WCP S2C event decode error");
                        return;
                    }
                    rfl::visit(
                        [&](auto&& event) {
                            using Type = std::decay_t<decltype(event)>;
                            try {
                                if constexpr (std::is_same<Type, wcp::WaveformsLoaded>()) {
                                    onWaveformsLoaded(event);
                                }
                                else if constexpr (std::is_same<Type, wcp::GotoDeclaration>()) {
                                    onGotoDeclaration(event);
                                }
                                else if constexpr (std::is_same<Type, wcp::AddDrivers>()) {
                                    onAddDrivers(event);
                                }
                                else if constexpr (std::is_same<Type, wcp::AddLoads>()) {
                                    onAddLoads(event);
                                }
                            }
                            catch (const std::exception& e) {
                                ERROR("WCP S2C Error: {} {}", *message, e.what());
                            }
                        },
                        *s2cEvent);
                }
            },
            *s2cMessage);
    }

    stop();
}

void waves::WcpClient::onWaveformsLoaded(const wcp::WaveformsLoaded& waveformsLoaded) {
    m_lspServer->onWaveformLoaded(waveformsLoaded.source);
}

void waves::WcpClient::onGotoDeclaration(const wcp::GotoDeclaration& gotoDeclaration) {
    m_lspServer->onGotoDeclaration(gotoDeclaration.variable);
}

void waves::WcpClient::onAddDrivers(const wcp::AddDrivers& addDrivers) {
    sendMessage(rfl::to_generic<rfl::UnderlyingEnums>(wcp::AddVariables{
        .type = "command",
        .command = "add_variables",
        .variables = m_lspServer->getDrivers(addDrivers.variable),
    }));
}

void waves::WcpClient::onAddLoads(const wcp::AddLoads& addLoads) {
    sendMessage(rfl::to_generic<rfl::UnderlyingEnums>(wcp::AddVariables{
        .type = "command",
        .command = "add_variables",
        .variables = m_lspServer->getLoads(addLoads.variable),
    }));
}

void waves::WcpClient::addItem(const waves::ItemToWaveform& params) {
    sendMessage(rfl::to_generic<rfl::UnderlyingEnums>(wcp::AddItems{
        .type = "command",
        .command = "add_items",
        .items = {params.path},
        .recursive = params.recursive,
    }));
}

void waves::WcpClient::loadWaveform(const std::string& source) {
    sendMessage(rfl::to_generic<rfl::UnderlyingEnums>(wcp::Load{
        .type = "command",
        .command = "load",
        .source = source,
    }));
}
