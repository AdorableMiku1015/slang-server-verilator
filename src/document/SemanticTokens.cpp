//------------------------------------------------------------------------------
// SemanticTokens.cpp
// Token classification for LSP semantic tokens
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "document/SemanticTokens.h"

#include "document/ShallowAnalysis.h"
#include "document/SymbolIndexer.h"
#include "document/SyntaxIndexer.h"
#include "util/Logging.h"
#include <algorithm>
#include <iterator>
#include <optional>

#include "slang/ast/SemanticFacts.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/parsing/Token.h"
#include "slang/parsing/TokenKind.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"

namespace server {

using namespace slang;

namespace {

constexpr std::string_view TYPE_NAMES[] = {
    "namespace", "class",         "interface", "enum",     "enumMember", "struct",
    "type",      "typeParameter", "parameter", "variable", "property",   "function",
    "macro",     "label",         "net",       "port",     "instance",   "modport",
};

constexpr std::string_view MODIFIER_NAMES[] = {
    "declaration",
    "definition",
    "readonly",
    "defaultLibrary",
};

static_assert(std::size(TYPE_NAMES) == static_cast<size_t>(SemanticTokenType::Count));
static_assert(std::size(MODIFIER_NAMES) == static_cast<size_t>(SemanticTokenModifier::Count));

/// Ordering used when one source token maps to more than one symbol, like the shorthand
/// `.name` port connection, which denotes both the formal port and the value connected to
/// it. Lower wins; the exact order is a presentation choice, not a correctness one.
int tokenTypePriority(SemanticTokenType type) {
    switch (type) {
        case SemanticTokenType::Port:
            return 0;
        case SemanticTokenType::Modport:
            return 1;
        case SemanticTokenType::Net:
            return 2;
        case SemanticTokenType::Variable:
            return 3;
        case SemanticTokenType::Parameter:
            return 4;
        case SemanticTokenType::EnumMember:
            return 5;
        case SemanticTokenType::Property:
            return 6;
        case SemanticTokenType::TypeParameter:
            return 7;
        case SemanticTokenType::Type:
        case SemanticTokenType::Enum:
        case SemanticTokenType::Struct:
        case SemanticTokenType::Class:
        case SemanticTokenType::Interface:
        case SemanticTokenType::Namespace:
            return 8;
        case SemanticTokenType::Function:
            return 9;
        case SemanticTokenType::Instance:
            return 10;
        case SemanticTokenType::Label:
        case SemanticTokenType::Macro:
            return 11;
        default:
            return 12;
    }
}

/// LSP positions are counted in UTF-16 code units, so multi byte characters need to be
/// measured individually. A four byte sequence is a surrogate pair, which is two units.
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

bool positionLess(const lsp::Position& left, const lsp::Position& right) {
    return left.line != right.line ? left.line < right.line : left.character < right.character;
}

/// True when the token is the qualifier of a `::` name, like the package in `pkg::x`.
bool isScopeQualifier(const syntax::SyntaxNode* node, const parsing::Token& token) {
    // `pkg::x` is a ScopedName whose left side is the qualifier, but the token's parent
    // is the IdentifierName that wraps it
    for (int depth = 0; node && depth < 2; depth++, node = node->parent) {
        if (node->kind == syntax::SyntaxKind::ScopedName) {
            auto& scoped = node->as<syntax::ScopedNameSyntax>();
            if (scoped.separator.kind == parsing::TokenKind::DoubleColon)
                return scoped.left->getFirstToken() == token;
        }
        else if (node->kind == syntax::SyntaxKind::PackageImportItem) {
            return node->as<syntax::PackageImportItemSyntax>().package == token;
        }
    }
    return false;
}

/// A token type with the modifiers that came along with it.
struct TokenClass {
    SemanticTokenType type;
    uint32_t modifiers = 0;

    /// The symbol the type was derived from, when there is one. Used to tell declarations
    /// apart from references to the same symbol.
    const ast::Symbol* symbol = nullptr;
};

std::optional<TokenClass> classifySymbol(const ast::Symbol& symbol) {
    auto result = [&](SemanticTokenType type) -> std::optional<TokenClass> {
        return TokenClass{.type = type, .symbol = &symbol};
    };

    switch (symbol.kind) {
        case ast::SymbolKind::Package:
        case ast::SymbolKind::ConfigBlock:
            return result(SemanticTokenType::Namespace);

        case ast::SymbolKind::Definition: {
            switch (symbol.as<ast::DefinitionSymbol>().definitionKind) {
                case ast::DefinitionKind::Module:
                case ast::DefinitionKind::Program:
                    return result(SemanticTokenType::Class);
                case ast::DefinitionKind::Interface:
                    return result(SemanticTokenType::Interface);
            }
            return std::nullopt;
        }

        case ast::SymbolKind::InterfacePort:
        case ast::SymbolKind::Port:
        case ast::SymbolKind::MultiPort:
        case ast::SymbolKind::PrimitivePort:
        case ast::SymbolKind::AssertionPort:
            return result(SemanticTokenType::Port);

        case ast::SymbolKind::Modport:
        case ast::SymbolKind::ModportPort:
        case ast::SymbolKind::ModportClocking:
        case ast::SymbolKind::ClockingBlock:
            return result(SemanticTokenType::Modport);

        case ast::SymbolKind::Net:
        case ast::SymbolKind::NetAlias:
            return result(SemanticTokenType::Net);

        case ast::SymbolKind::Variable:
        case ast::SymbolKind::ClockVar:
        case ast::SymbolKind::LocalAssertionVar:
        case ast::SymbolKind::Iterator:
            return result(SemanticTokenType::Variable);

        case ast::SymbolKind::Genvar: {
            auto cls = result(SemanticTokenType::Variable);
            cls->modifiers |= modifierBit(SemanticTokenModifier::ReadOnly);
            return cls;
        }

        case ast::SymbolKind::FormalArgument:
            return result(SemanticTokenType::Parameter);

        case ast::SymbolKind::Parameter: {
            auto cls = result(SemanticTokenType::Parameter);
            if (symbol.as<ast::ParameterSymbol>().isLocalParam())
                cls->modifiers |= modifierBit(SemanticTokenModifier::ReadOnly);
            return cls;
        }

        case ast::SymbolKind::Specparam:
        case ast::SymbolKind::DefParam:
            return result(SemanticTokenType::Parameter);

        case ast::SymbolKind::EnumValue:
            return result(SemanticTokenType::EnumMember);

        case ast::SymbolKind::EnumType:
            return result(SemanticTokenType::Enum);

        case ast::SymbolKind::PackedStructType:
        case ast::SymbolKind::UnpackedStructType:
        case ast::SymbolKind::PackedUnionType:
        case ast::SymbolKind::UnpackedUnionType:
            return result(SemanticTokenType::Struct);

        case ast::SymbolKind::ClassType:
        case ast::SymbolKind::GenericClassDef:
        case ast::SymbolKind::CovergroupType:
            return result(SemanticTokenType::Class);

        case ast::SymbolKind::TypeAlias:
        case ast::SymbolKind::ForwardingTypedef:
            return result(SemanticTokenType::Type);

        case ast::SymbolKind::TypeParameter:
            return result(SemanticTokenType::TypeParameter);

        case ast::SymbolKind::Field:
        case ast::SymbolKind::ClassProperty:
        case ast::SymbolKind::PatternVar:
        case ast::SymbolKind::ConstraintBlock:
            return result(SemanticTokenType::Property);

        case ast::SymbolKind::Subroutine:
        case ast::SymbolKind::MethodPrototype:
        case ast::SymbolKind::LetDecl:
        case ast::SymbolKind::Sequence:
        case ast::SymbolKind::Property:
            return result(SemanticTokenType::Function);

        case ast::SymbolKind::Instance:
        case ast::SymbolKind::InstanceArray:
        case ast::SymbolKind::PrimitiveInstance:
        case ast::SymbolKind::CheckerInstance:
        case ast::SymbolKind::CheckerInstanceBody:
            return result(SemanticTokenType::Instance);

        case ast::SymbolKind::InstanceBody:
            // A body is indexed at its definition's name, so it stands for the definition
            return classifySymbol(symbol.as<ast::InstanceBodySymbol>().getDefinition());

        default:
            return std::nullopt;
    }
}

} // namespace

std::span<const std::string_view> semanticTokenTypeNames() {
    return TYPE_NAMES;
}

std::span<const std::string_view> semanticTokenModifierNames() {
    return MODIFIER_NAMES;
}

bool isCustomSemanticTokenType(SemanticTokenType type) {
    return type >= SemanticTokenType::Net && type < SemanticTokenType::Count;
}

/// Walks the tokens of a document once and classifies the ones that carry meaning. The
/// symbol index is keyed by declaration tokens, so most declarations resolve with a
/// single hash lookup; tokens that syntax identifies outright are handled next, and
/// everything else falls back to a lookup of the name in the enclosing scope.
class SemanticTokenCollector {
public:
    SemanticTokenCollector(const ShallowAnalysis& analysis, const lsp::RequestContext& ctx) :
        m_analysis(analysis), m_ctx(ctx), m_syntaxes(analysis.syntaxes),
        m_symbols(analysis.m_symbolIndexer) {
        // Disabled regions come out in visit order, which is not necessarily sorted
        m_disabledRanges = m_syntaxes.disabledRegions;
        std::ranges::sort(m_disabledRanges, {},
                          [](const SourceRange& range) { return range.start().offset(); });
    }

    std::vector<SemanticToken> result;

    void collectTokens() {
        const auto& tokens = m_syntaxes.collected;
        result.reserve(tokens.size() / 4);

        size_t counter = 0;
        for (const auto* token : tokens) {
            // Checking every token would dominate the cost of a small file
            if ((++counter % 64) == 0)
                m_ctx.throwIfCancelled("semantic tokens");

            if (!token || token->isMissing())
                continue;

            switch (token->kind) {
                case parsing::TokenKind::Identifier:
                case parsing::TokenKind::SystemIdentifier:
                case parsing::TokenKind::MacroUsage:
                    break;
                default:
                    continue;
            }

            auto text = token->rawText();
            if (text.empty())
                continue;

            // Tokens in disabled `ifdef regions are rendered dimmed by the client, so
            // highlighting them would fight with that
            if (isInactive(*token))
                continue;

            auto offset = static_cast<uint32_t>(token->location().offset());
            // Escaped identifiers are written with a leading backslash and a trailing space,
            // neither of which belongs in the highlighted range
            if (text.size() > 1 && text.front() == '\\') {
                text.remove_prefix(1);
                offset += 1;
                while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                    text.remove_suffix(1);
                if (text.empty())
                    continue;
            }

            auto cls = classifyToken(*token);
            if (!cls)
                continue;

            result.push_back(SemanticToken{
                .offset = offset,
                .length = static_cast<uint32_t>(text.size()),
                .type = cls->type,
                .modifiers = cls->modifiers,
            });
        }
    }

private:
    const ShallowAnalysis& m_analysis;
    const lsp::RequestContext& m_ctx;
    const SyntaxIndexer& m_syntaxes;
    const SymbolIndexer& m_symbols;
    std::vector<SourceRange> m_disabledRanges;

    bool isInactive(const parsing::Token& token) const {
        auto offset = token.location().offset();
        // Ranges are sorted by start, so only the last one starting at or before this
        // token can contain it
        auto it = std::partition_point(m_disabledRanges.begin(), m_disabledRanges.end(),
                                       [&](const SourceRange& range) {
                                           return range.start().offset() <= offset;
                                       });
        if (it == m_disabledRanges.begin())
            return false;

        return std::prev(it)->end().offset() > offset;
    }

    std::optional<TokenClass> classifyToken(const parsing::Token& token) {
        if (token.kind == parsing::TokenKind::MacroUsage) {
            return TokenClass{.type = SemanticTokenType::Macro};
        }
        if (token.kind == parsing::TokenKind::SystemIdentifier) {
            return TokenClass{.type = SemanticTokenType::Function,
                              .modifiers = modifierBit(SemanticTokenModifier::DefaultLibrary)};
        }

        // A shorthand `.name` connection names the formal port, but the symbol index
        // records the symbols it connects, which are nets and variables
        if (isShorthandPortConnection(token))
            return TokenClass{.type = SemanticTokenType::Port};

        if (auto cls = classifyIndexed(token))
            return cls;
        // Syntax that names something outright wins over a name lookup, which could find
        // an unrelated symbol that happens to share the name
        if (auto cls = classifySyntax(token))
            return cls;
        return classifyReference(token);
    }

    /// True for the name of a `.name` port connection, which has no parentheses and so
    /// denotes both the formal port and the signal connected to it.
    bool isShorthandPortConnection(const parsing::Token& token) const {
        auto* parent = m_syntaxes.getTokenParent(&token);
        if (!parent || parent->kind != syntax::SyntaxKind::NamedPortConnection)
            return false;

        auto& connection = parent->as<syntax::NamedPortConnectionSyntax>();
        return !connection.openParen && connection.name == token;
    }

    /// The token is part of a declaration or instantiation the symbol indexer recorded.
    std::optional<TokenClass> classifyIndexed(const parsing::Token& token) const {
        auto symbols = m_symbols.getSymbols(&token);
        if (symbols.empty())
            return std::nullopt;

        std::optional<TokenClass> best;
        for (auto* symbol : symbols) {
            if (!symbol)
                continue;

            auto cls = classifySymbol(*symbol);
            if (!cls)
                continue;

            if (!best || tokenTypePriority(cls->type) < tokenTypePriority(best->type))
                best = cls;
        }

        if (!best)
            return std::nullopt;

        if (best->symbol->location == token.location()) {
            best->modifiers |= modifierBit(SemanticTokenModifier::Declaration);
            if (best->symbol->kind == ast::SymbolKind::Definition)
                best->modifiers |= modifierBit(SemanticTokenModifier::Definition);
        }
        return best;
    }

    /// Resolve the token the way any reference is resolved: look the name up in the
    /// nearest enclosing scope.
    std::optional<TokenClass> classifyReference(const parsing::Token& token) const {
        auto* parent = m_syntaxes.getTokenParent(&token);
        if (!parent)
            return std::nullopt;

        auto name = token.valueText();
        if (name.empty())
            return std::nullopt;

        if (auto* scope = findEnclosingScope(*parent)) {
            if (auto* symbol = scope->lookupName(name)) {
                if (auto cls = classifySymbol(*symbol))
                    return cls;
            }
        }

        // Module, interface and package names live outside of any local scope. A name that
        // qualifies a `::` name can only be a package, so prefer that for those.
        bool qualifier = isScopeQualifier(parent, token);
        if (qualifier) {
            if (auto* package = m_analysis.getCompilation()->getPackage(name)) {
                return TokenClass{.type = SemanticTokenType::Namespace, .symbol = package};
            }
        }
        if (auto* definition = m_analysis.getDefinition(name)) {
            if (auto cls = classifySymbol(*definition))
                return cls;
        }
        if (auto* package = m_analysis.getCompilation()->getPackage(name)) {
            return TokenClass{.type = SemanticTokenType::Namespace, .symbol = package};
        }
        return std::nullopt;
    }

    /// Names that no symbol describes, but that the surrounding syntax identifies.
    std::optional<TokenClass> classifySyntax(const parsing::Token& token) const {
        auto* parent = m_syntaxes.getTokenParent(&token);
        if (!parent)
            return std::nullopt;

        switch (parent->kind) {
            case syntax::SyntaxKind::DefineDirective: {
                auto& define = parent->as<syntax::DefineDirectiveSyntax>();
                if (define.name == token) {
                    return TokenClass{.type = SemanticTokenType::Macro,
                                      .modifiers = modifierBit(SemanticTokenModifier::Declaration)};
                }
            } break;
            case syntax::SyntaxKind::NamedBlockClause: {
                auto& clause = parent->as<syntax::NamedBlockClauseSyntax>();
                if (clause.name == token)
                    return TokenClass{.type = SemanticTokenType::Label};
            } break;
            case syntax::SyntaxKind::HierarchicalInstance: {
                auto& instance = parent->as<syntax::HierarchicalInstanceSyntax>();
                if (instance.decl && instance.decl->name == token)
                    return TokenClass{.type = SemanticTokenType::Instance};
            } break;
            case syntax::SyntaxKind::NamedPortConnection: {
                // The explicit `.name(expr)` form is also indexed as a port, but an
                // unresolved connection isn't indexed at all
                auto& connection = parent->as<syntax::NamedPortConnectionSyntax>();
                if (connection.name == token)
                    return TokenClass{.type = SemanticTokenType::Port};
            } break;
            case syntax::SyntaxKind::ExplicitNonAnsiPort: {
                // `.name(expr)` in a non-ANSI port list declares a port, which won't be
                // found in the module's scope if something else there shares the name
                auto& port = parent->as<syntax::ExplicitNonAnsiPortSyntax>();
                if (port.name == token)
                    return TokenClass{.type = SemanticTokenType::Port};
            } break;
            default:
                break;
        }
        return std::nullopt;
    }

    /// The scope a token sits in, found by walking up to the nearest syntax node that
    /// maps to a symbol.
    const ast::Scope* findEnclosingScope(const syntax::SyntaxNode& syntax) const {
        for (auto* node = &syntax; node; node = node->parent) {
            auto it = m_symbols.syntex.find(node);
            if (it == m_symbols.syntex.end())
                continue;

            for (auto* symbol : it->second) {
                if (!symbol)
                    continue;
                if (symbol->isScope())
                    return &symbol->as<ast::Scope>();

                // A definition's members are in its elaborated body, not in the scope
                // that contains the definition itself
                if (symbol->kind == ast::SymbolKind::Definition)
                    continue;
                if (auto* scope = symbol->getParentScope())
                    return scope;
            }
        }
        return nullptr;
    }
};

std::vector<SemanticToken> collectSemanticTokens(const ShallowAnalysis& analysis,
                                                 const lsp::RequestContext& ctx) {
    SemanticTokenCollector collector(analysis, ctx);
    collector.collectTokens();
    return std::move(collector.result);
}

std::vector<uint32_t> encodeSemanticTokens(std::string_view text,
                                           std::span<const SemanticToken> tokens,
                                           const lsp::Range* range,
                                           const lsp::RequestContext& ctx) {
    std::vector<uint32_t> data;
    if (tokens.empty())
        return data;

    std::vector<size_t> lineOffsets;
    SourceManager::computeLineOffsets(text, lineOffsets);
    if (lineOffsets.empty())
        return data;

    auto lineOf = [&](size_t offset) {
        auto it = std::upper_bound(lineOffsets.begin(), lineOffsets.end(), offset);
        return static_cast<size_t>(it - lineOffsets.begin()) - 1;
    };

    data.reserve(tokens.size() * 5);

    // The encoding is relative to the previous token, so these only advance for tokens
    // that are actually emitted
    uint32_t prevLine = 0;
    uint32_t prevChar = 0;
    size_t counter = 0;

    for (const auto& token : tokens) {
        if ((++counter % 256) == 0)
            ctx.throwIfCancelled("encode semantic tokens");

        if (token.length == 0 || token.offset + token.length > text.size())
            continue;

        auto tokenText = text.substr(token.offset, token.length);
        // The protocol has no way to describe a token that crosses a line boundary
        if (tokenText.find_first_of("\r\n") != std::string_view::npos)
            continue;

        auto line = lineOf(token.offset);
        auto lineStart = lineOffsets[line];
        auto character = utf16Length(text.substr(lineStart, token.offset - lineStart));
        auto length = utf16Length(tokenText);

        if (range) {
            lsp::Position start{.line = static_cast<lsp::uint>(line),
                                .character = static_cast<lsp::uint>(character)};
            if (positionLess(start, range->start) || !positionLess(start, range->end))
                continue;
        }

        data.push_back(static_cast<uint32_t>(line) - prevLine);
        data.push_back(line == prevLine ? character - prevChar : character);
        data.push_back(length);
        data.push_back(static_cast<uint32_t>(token.type));
        data.push_back(token.modifiers);

        prevLine = static_cast<uint32_t>(line);
        prevChar = character;
    }

    // A partially emitted response would corrupt everything the client decodes after it
    if (ctx.isCancelled())
        throw lsp::RequestCancelled("encode semantic tokens");

    return data;
}

} // namespace server
