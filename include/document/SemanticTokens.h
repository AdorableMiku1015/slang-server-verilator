//------------------------------------------------------------------------------
// SemanticTokens.h
// Classifies document tokens into LSP semantic tokens for syntax highlighting
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include "lsp/LspTypes.h"
#include "lsp/RequestContext.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace slang {
class SourceManager;
}

namespace server {

class ShallowAnalysis;

/// The semantic token types of our legend. The declaration order defines the indices
/// reported to the client, so entries may only be appended, never reordered or removed.
///
/// Everything up to (and including) `Label` is part of the standard LSP type set and is
/// colored by any theme out of the box. The remaining types describe SystemVerilog
/// concepts that the standard set cannot express; clients map them onto TextMate scopes
/// via the `semanticTokenScopes` contribution (see the vscode client's package.json).
enum class SemanticTokenType : uint8_t {
    Namespace,
    Class,
    Interface,
    Enum,
    EnumMember,
    Struct,
    Type,
    TypeParameter,
    Parameter,
    Variable,
    Property,
    Function,
    Macro,
    Label,
    Net,
    Port,
    Instance,
    Modport,

    /// Not a token type; the number of entries in the legend.
    Count,
};

/// The semantic token modifiers of our legend; same ordering rules as SemanticTokenType.
enum class SemanticTokenModifier : uint8_t {
    Declaration,
    Definition,
    ReadOnly,
    DefaultLibrary,

    /// Not a modifier; the number of entries in the legend.
    Count,
};

/// The names reported to the client for each SemanticTokenType.
std::span<const std::string_view> semanticTokenTypeNames();

/// The names reported to the client for each SemanticTokenModifier.
std::span<const std::string_view> semanticTokenModifierNames();

/// True if the type is not part of the standard LSP set, so clients need a scope mapping
/// to color it.
bool isCustomSemanticTokenType(SemanticTokenType type);

/// The bit for a modifier in SemanticToken::modifiers.
constexpr uint32_t modifierBit(SemanticTokenModifier modifier) {
    return 1u << static_cast<uint8_t>(modifier);
}

/// A token with a semantic meaning, still in document byte offsets. The LSP wire format
/// (relative line/character offsets in UTF-16 code units) is produced by
/// encodeSemanticTokens.
struct SemanticToken {
    /// Byte offset of the token in the document text.
    uint32_t offset;

    /// Length of the token in bytes.
    uint32_t length;

    SemanticTokenType type;

    /// A bitset of modifiers, built with modifierBit.
    uint32_t modifiers = 0;
};

/// Classify every token of the document. Only hash based lookups are used, so this is
/// cheap enough to run once per document change; the result is cached by ShallowAnalysis.
std::vector<SemanticToken> collectSemanticTokens(const ShallowAnalysis& analysis,
                                                 const lsp::RequestContext& ctx = {});

/// Encode classified tokens into the LSP relative format (groups of five integers).
/// Tokens that span multiple lines are skipped, and when `range` is given only tokens
/// starting inside it are emitted.
/// @param text the document text, including slang's trailing nul terminator
std::vector<uint32_t> encodeSemanticTokens(std::string_view text,
                                           std::span<const SemanticToken> tokens,
                                           const lsp::Range* range = nullptr,
                                           const lsp::RequestContext& ctx = {});

} // namespace server
