// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "util/Converters.h"
#include <catch2/catch_test_macros.hpp>

#include "slang/text/SourceManager.h"

TEST_CASE("LSP positions use raw source lines") {
    slang::SourceManager sourceManager;
    auto buffer = sourceManager.assignText("source.sv", "first\n`line\nthird\n");
    REQUIRE(buffer);

    auto directive = sourceManager.getSourceLocation(buffer.id, 2, 1);
    auto location = sourceManager.getSourceLocation(buffer.id, 3, 3);
    REQUIRE(directive);
    REQUIRE(location);
    sourceManager.addLineDirective(*directive, 100, "mapped.sv", 0);

    CHECK(sourceManager.getLineNumber(*location) == 100);
    auto position = server::toPosition(*location, sourceManager);
    CHECK(position.line == 2);
    CHECK(position.character == 2);

    auto range = server::toRange(*location, sourceManager, 2);
    CHECK(range.start.line == 2);
    CHECK(range.start.character == 2);
    CHECK(range.end.line == 2);
    CHECK(range.end.character == 4);
}

TEST_CASE("LSP positions resolve macro locations") {
    slang::SourceManager sourceManager;
    auto buffer = sourceManager.assignText("source.sv", "first\nmacro(D)\n");
    REQUIRE(buffer);

    auto location = sourceManager.getSourceLocation(buffer.id, 2, 7);
    REQUIRE(location);
    slang::SourceRange expansionRange(*location, *location + 1);
    auto macroLocation = sourceManager.createExpansionLoc(*location, expansionRange, "M");

    CHECK(sourceManager.getRawLineNumber(macroLocation) == 0);
    auto position = server::toPosition(macroLocation, sourceManager);
    CHECK(position.line == 1);
    CHECK(position.character == 6);

    auto range = server::toRange(slang::SourceRange(macroLocation, macroLocation + 1),
                                 sourceManager);
    CHECK(range.start.line == 1);
    CHECK(range.start.character == 6);
    CHECK(range.end.line == 1);
    CHECK(range.end.character == 7);

    auto lengthRange = server::toRange(macroLocation, sourceManager, 1);
    CHECK(lengthRange.start.line == 1);
    CHECK(lengthRange.start.character == 6);
    CHECK(lengthRange.end.line == 1);
    CHECK(lengthRange.end.character == 7);
}

TEST_CASE("LSP positions count UTF-16 code units") {
    slang::SourceManager sourceManager;
    // 中文 is three bytes per character but one UTF-16 unit, and the emoji is a surrogate pair
    auto buffer = sourceManager.assignText("source.sv", "// 中文 😀\nlogic x;\n");
    REQUIRE(buffer);

    CHECK(server::utf16Length("// 中文 😀") == 8);
    CHECK(server::utf16ToByteOffset("// 中文 😀", 0) == 0);
    CHECK(server::utf16ToByteOffset("// 中文 😀", 4) == 6);
    CHECK(server::utf16ToByteOffset("// 中文 😀", 8) == 14);
    CHECK(server::utf16ToByteOffset("// 中文 😀", 9) == std::nullopt);
    // Columns inside a surrogate pair do not exist
    CHECK(server::utf16ToByteOffset("😀", 1) == std::nullopt);

    // The line is 14 bytes long, but a client sees 8 columns
    auto lineEnd = sourceManager.getSourceLocation(buffer.id, 1, 15);
    REQUIRE(lineEnd);
    auto position = server::toPosition(*lineEnd, sourceManager);
    CHECK(position.line == 0);
    CHECK(position.character == 8);

    // Round trip, and reject columns past the end of the line
    auto location = server::toSourceLocation(buffer.id, position, sourceManager);
    REQUIRE(location);
    CHECK(location->offset() == lineEnd->offset());
    CHECK_FALSE(server::toSourceLocation(buffer.id, lsp::Position{0, 9}, sourceManager));

    auto secondLine = sourceManager.getSourceLocation(buffer.id, 2, 6);
    REQUIRE(secondLine);
    auto roundTripped = server::toSourceLocation(buffer.id, lsp::Position{1, 5}, sourceManager);
    REQUIRE(roundTripped);
    CHECK(roundTripped->offset() == secondLine->offset());
}

TEST_CASE("LSP ranges measure multi byte tokens") {
    slang::SourceManager sourceManager;
    auto buffer = sourceManager.assignText("source.sv", "logic 名字;\n");
    REQUIRE(buffer);

    // `toRange` takes a byte length, so a three byte token is three columns wide here
    auto name = sourceManager.getSourceLocation(buffer.id, 1, 7);
    REQUIRE(name);
    auto range = server::toRange(*name, sourceManager, 6);
    CHECK(range.start.line == 0);
    CHECK(range.start.character == 6);
    CHECK(range.end.line == 0);
    CHECK(range.end.character == 8);
}
