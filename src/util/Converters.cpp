//------------------------------------------------------------------------------
// Converters.cpp
// Type conversion utilities for LSP server, primarily between slang and LSP types
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "util/Converters.h"

#include <algorithm>
#include <fmt/format.h>

#include "slang/text/SourceLocation.h"

namespace server {

using namespace slang;

// Runs dfs on the syntax node to find the name token, which will point to the same memory
std::optional<const parsing::Token> findNameToken(const syntax::SyntaxNode* node,
                                                  std::string_view name) {
    // The name and token will occupy the same memory, so match on that
    for (size_t i = 0; i < node->getChildCount(); i++) {
        // check if the child is a token
        auto token = node->childToken(i);
        if (token) {
            if (token.valueText().data() == name.data()) {
                return token;
            }
            continue;
        }
        // check if the child is a node
        auto child = node->childNode(i);
        if (child) {
            auto token = findNameToken(child, name);
            if (token)
                return token;
        }
    }
    return std::nullopt;
}

uint32_t utf16Length(std::string_view text) {
    uint32_t count = 0;
    for (size_t i = 0; i < text.size();) {
        auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            i += 1;
            count += 1;
        }
        else if ((byte & 0xE0) == 0xC0) {
            i += std::min<size_t>(2, text.size() - i);
            count += 1;
        }
        else if ((byte & 0xF0) == 0xE0) {
            i += std::min<size_t>(3, text.size() - i);
            count += 1;
        }
        else if ((byte & 0xF8) == 0xF0) {
            i += std::min<size_t>(4, text.size() - i);
            count += 2;
        }
        else {
            // Invalid byte; treat it as a single unit so we always make progress
            i += 1;
            count += 1;
        }
    }
    return count;
}

std::optional<size_t> utf16ToByteOffset(std::string_view line, uint32_t column) {
    uint32_t count = 0;
    for (size_t i = 0; i < line.size();) {
        if (count == column)
            return i;

        auto byte = static_cast<unsigned char>(line[i]);
        size_t length = 1;
        uint32_t units = 1;
        if (byte < 0x80) {
            length = 1;
        }
        else if ((byte & 0xE0) == 0xC0) {
            length = 2;
        }
        else if ((byte & 0xF0) == 0xE0) {
            length = 3;
        }
        else if ((byte & 0xF8) == 0xF0) {
            length = 4;
            units = 2;
        }

        i += std::min(length, line.size() - i);
        count += units;
    }

    // A column at the very end of the line addresses its end
    if (count == column)
        return line.size();
    return std::nullopt;
}

namespace {

/// Position of `offset` (a byte offset in the buffer of `actualLoc`) in the expanded file
lsp::Position toPositionAt(const SourceLocation& actualLoc, size_t offset,
                           const SourceManager& sourceManager) {
    auto text = sourceManager.getSourceText(actualLoc.buffer());
    if (text.empty())
        return lsp::Position{.line = 0, .character = 0};

    // Offsets in here are otherwise handed to slang, which requires them to be in range
    auto location = SourceLocation(actualLoc.buffer(), std::min(offset, text.size() - 1));
    auto byteColumn = sourceManager.getColumnNumber(location);
    size_t character = byteColumn > 0 ? byteColumn - 1 : 0;

    // The column is a byte offset into the line, but LSP counts UTF-16 code units
    if (character <= location.offset())
        character = utf16Length(text.substr(location.offset() - character, character));

    return lsp::Position{.line = static_cast<lsp::uint>(sourceManager.getRawLineNumber(location) -
                                                        1),
                         .character = static_cast<lsp::uint>(character)};
}

} // namespace

lsp::Position toPosition(const SourceLocation& loc, const SourceManager& sourceManager) {
    auto actualLoc = sourceManager.getFullyExpandedLoc(loc);
    return toPositionAt(actualLoc, actualLoc.offset(), sourceManager);
}

std::optional<SourceLocation> toSourceLocation(BufferID buffer, const lsp::Position& position,
                                               const SourceManager& sourceManager) {
    auto lineStartLoc = sourceManager.getSourceLocation(buffer, position.line + 1, 1);
    if (!lineStartLoc)
        return std::nullopt;

    auto text = sourceManager.getSourceText(buffer);
    size_t lineStart = lineStartLoc->offset();
    if (lineStart > text.size())
        return std::nullopt;

    size_t lineEnd = lineStart;
    while (lineEnd < text.size() && text[lineEnd] != '\n' && text[lineEnd] != '\r')
        lineEnd++;

    auto byteOffset = utf16ToByteOffset(text.substr(lineStart, lineEnd - lineStart),
                                        position.character);
    if (!byteOffset)
        return std::nullopt;
    return SourceLocation(buffer, lineStart + *byteOffset);
}

lsp::Range toRange(const SourceRange& range, const SourceManager& sourceManager) {
    auto actualRange = sourceManager.getFullyExpandedRange(range);
    return lsp::Range{.start = toPosition(actualRange.start(), sourceManager),
                      .end = toPosition(actualRange.end(), sourceManager)};
}

lsp::Location toOriginalLocation(const SourceRange& range, const SourceManager& sourceManager) {
    auto origRange = sourceManager.getFullyOriginalRange(range);
    return lsp::Location{
        .uri = URI::fromFile(sourceManager.getFullPath(origRange.start().buffer())),
        .range = toRange(origRange, sourceManager),
    };
}

lsp::Range toRange(const SourceLocation& loc, const SourceManager& sourceManager,
                   const size_t length) {

    // `length` is a byte count, so the end has to be measured rather than added to the start
    // column: LSP columns count UTF-16 code units.
    auto actualLoc = sourceManager.getFullyExpandedLoc(loc);
    return lsp::Range{.start = toPositionAt(actualLoc, actualLoc.offset(), sourceManager),
                      .end = toPositionAt(actualLoc, actualLoc.offset() + length, sourceManager)};
}

lsp::Location toLocation(const SourceRange& range, const SourceManager& sourceManager) {
    auto declRange = sourceManager.getFullyExpandedRange(range);

    return lsp::Location{.uri = URI::fromFile(
                             sourceManager.getFullPath(declRange.start().buffer())),
                         .range = toRange(declRange, sourceManager)};
}

lsp::Location toLocation(const SourceLocation& loc, const SourceManager& sourceManager) {
    auto actualLoc = sourceManager.getFullyExpandedLoc(loc);
    return lsp::Location{.uri = URI::fromFile(sourceManager.getFullPath(actualLoc.buffer())),
                         .range = lsp::Range{.start = toPosition(actualLoc, sourceManager),
                                             .end = toPosition(actualLoc, sourceManager)}};
}

lsp::SymbolKind toSymbolKind(const syntax::SyntaxKind& kind) {
    switch (kind) {
        case syntax::SyntaxKind::InterfaceDeclaration:
            return lsp::SymbolKind::Interface;
        case syntax::SyntaxKind::ModuleDeclaration:
        case syntax::SyntaxKind::CheckerDeclaration:
        case syntax::SyntaxKind::ProgramDeclaration:
            return lsp::SymbolKind::Module;
        case syntax::SyntaxKind::PackageDeclaration:
            return lsp::SymbolKind::Package;
        case syntax::SyntaxKind::ClassDeclaration:
            return lsp::SymbolKind::Class;
        case syntax::SyntaxKind::FunctionDeclaration:
        case syntax::SyntaxKind::TaskDeclaration:
            return lsp::SymbolKind::Function;
        default:
            return lsp::SymbolKind::Null;
    }
}

lsp::MarkupContent markdown(std::string& md) {
    return lsp::MarkupContent{.kind = lsp::MarkupKindOptions::from_name<"markdown">().str(),
                              .value = md};
}

std::string subroutineString(ast::SubroutineKind kind) {
    switch (kind) {
        case ast::SubroutineKind::Function:
            return "function";
        case ast::SubroutineKind::Task:
            return "task";
        default:
            SLANG_UNREACHABLE;
    }
}

} // namespace server
