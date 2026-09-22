//------------------------------------------------------------------------------
// Converters.h
// Type conversion utilities for LSP server, primarily between slang and LSP types
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "lsp/LspTypes.h"
#include <cstdint>
#include <optional>
#include <string_view>

#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Symbol.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"

namespace server {

using namespace slang;

std::optional<const parsing::Token> findNameToken(const syntax::SyntaxNode* node,
                                                  std::string_view name);

/// LSP positions are counted in UTF-16 code units, so multi byte characters need to be
/// measured individually. A four byte sequence is a surrogate pair, which is two units.
uint32_t utf16Length(std::string_view text);

/// Byte offset within `line` (which must not include a line terminator) of the given UTF-16
/// column. Returns nullopt if the column is past the end of the line.
std::optional<size_t> utf16ToByteOffset(std::string_view line, uint32_t column);

lsp::Position toPosition(const SourceLocation& loc, const SourceManager& sourceManager);

std::optional<SourceLocation> toSourceLocation(BufferID buffer, const lsp::Position& position,
                                               const SourceManager& sourceManager);

lsp::Range toRange(const SourceRange& range, const SourceManager& sourceManager);

lsp::Location toOriginalLocation(const SourceRange& range, const SourceManager& sourceManager);

lsp::Range toRange(const SourceLocation& loc, const SourceManager& sourceManager,
                   const size_t length);

lsp::Location toLocation(const SourceRange& range, const SourceManager& sourceManager);

lsp::Location toLocation(const SourceLocation& loc, const SourceManager& sourceManager);

lsp::SymbolKind toSymbolKind(const slang::syntax::SyntaxKind& kind);

/// Completion item kind for a symbol. Grouped the same way as the semantic token types so that both
/// features describe a symbol consistently: ports, parameters, instances and so on are not all
/// "property", and a data port is not an interface.
lsp::CompletionItemKind toCompletionItemKind(const slang::ast::Symbol& symbol);

lsp::MarkupContent markdown(std::string& md);

std::string portString(ast::ArgumentDirection dir);

std::string subroutineString(ast::SubroutineKind kind);

} // namespace server
