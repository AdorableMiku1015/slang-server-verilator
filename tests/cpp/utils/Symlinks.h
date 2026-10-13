// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
#pragma once

/// Helpers for tests that use a directory symlink as a fixture.
///
/// Creating one needs Developer Mode or an elevated token on Windows, so a machine without it
/// has to skip those tests instead of failing on the privilege error. Unix has no such
/// restriction, so everything here is about reporting what the filesystem refused.

#include "utils/Utils.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <system_error>

/// Creates a directory symlink, returning the error the filesystem reported, if any. A caller
/// that has a meaningful fallback can keep going without the alias.
inline std::error_code createDirectorySymlink(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return ec;
}

/// Skips the current test when the link cannot be created.
///
/// This is a macro so that Catch2 reports the caller's file and line. It also has to be used
/// before any section runs: Catch2 runs the test body once more without sections after a section
/// skips, which would run the assertions that follow the sections against an unset fixture.
#define REQUIRE_DIRECTORY_SYMLINK(target, link)                      \
    do {                                                             \
        if (auto ec = createDirectorySymlink(target, link))          \
            SKIP("Directory symlinks unavailable: " + ec.message()); \
    } while (false)
