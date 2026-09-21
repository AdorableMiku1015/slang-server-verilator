//------------------------------------------------------------------------------
// server_main.cpp
// Generates C++ headers for SystemVerilog types
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#ifdef _WIN32
#    include <fcntl.h>
#    include <io.h>
#endif

#include "SlangServer.h"
#include <cstdio>
#include <exception>
#include <fmt/format.h>
#include <rfl/DefaultIfMissing.hpp>

#include "slang/util/CommandLine.h"
#include "slang/util/VersionInfo.h"

using namespace slang;
using namespace server;

namespace {

/// Anything that terminates the process - an exception escaping a thread, a `noexcept` violation,
/// a throwing destructor - used to abort with nothing in the log to say why. Record the reason
/// before aborting so a crash in the field is diagnosable.
void logTerminateAndAbort() {
    // Deliberately avoid allocation and formatting while unwinding
    if (auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& e) {
            std::fputs("FATAL: terminating: ", stderr);
            std::fputs(e.what(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
            std::abort();
        }
        catch (...) {
            std::fputs("FATAL: terminating with a non-standard exception\n", stderr);
            std::fflush(stderr);
            std::abort();
        }
    }
    std::fputs("FATAL: terminating without an active exception\n", stderr);
    std::fflush(stderr);
    std::abort();
}

} // namespace

int main(int argc, char** argv) {

    std::set_terminate(logTerminateAndAbort);

#ifdef _WIN32
    // By default windows accesses streams in text mode. This mostly means that
    // line feeds are converted to carriage return-line feed (CRLF), this messes
    // up the language server protocol. The combat this to solution is to explicitly
    // set the IO line feed to use binary mode instead of text mode.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    OS::setupConsole();

    CommandLine cmdline;

    std::optional<bool> showHelp;
    cmdline.add("-h,--help", showHelp, "Display available options");

    std::optional<bool> showVersion;
    cmdline.add("--version", showVersion, "Display version information and exit");

    std::optional<bool> configSchema;
    cmdline.add("--config-schema", configSchema, "Print json schema of config file and exit");

    cmdline.parse(argc, argv);

    if (showHelp == true) {
        OS::print(cmdline.getHelpText("Slang Language Server"));
        return 0;
    }

    if (showVersion == true) {
        OS::print(fmt::format("slang-server version {}.{}.{}+{}\n", VersionInfo::getMajor(),
                              VersionInfo::getMinor(), VersionInfo::getPatch(),
                              VersionInfo::getHash()));
        return 0;
    }

    if (configSchema == true) {
        try {
            const std::string schema = rfl::json::to_schema<Config, rfl::DefaultIfMissing>(
                rfl::json::pretty | YYJSON_WRITE_PRETTY_TWO_SPACES
                // Add this when comment support is added
                // , "@generated from `include/Config.h`"
            );
            OS::print(schema);
            OS::print("\n");
        }
        catch (const std::exception& e) {
            OS::print(fmt::format("Error generating config schema: {}\n", e.what()));
            return 1;
        }
        return 0;
    }
    SlangLspClient client;
    SlangServer server(client);
    server.run();

    return 0;
}
