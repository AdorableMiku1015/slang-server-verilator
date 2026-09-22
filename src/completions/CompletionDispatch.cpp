//------------------------------------------------------------------------------
// CompletionDispatch.cpp
// Completion site classification and shared query dispatch.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/CompletionDispatch.h"

#include "completions/AssignmentPatternCompletions.h"
#include "completions/InstanceCompletions.h"
#include "completions/MacroCompletions.h"
#include "completions/MemberCompletions.h"
#include "completions/SystemTaskCompletions.h"
#include "document/ShallowAnalysis.h"
#include "lsp/SnippetString.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include <algorithm>
#include <string>

#include "slang/parsing/Token.h"
#include "slang/parsing/TokenKind.h"
#include "slang/syntax/AllSyntax.h"

namespace server {

namespace completions {

const std::vector<std::string>& completionTriggerCharacters() {
    static const std::vector<std::string> triggerCharacters{
        "`", // macros
        "#", // hierarchical instantiation: modules and interfaces
        ".", // hierarchical references
        "(", // function calls
        ":", // package scope (::), wire width
        "[", // wire width, array indexing
        "$", // system tasks and functions
        "{", // assignment patterns
    };
    return triggerCharacters;
}

} // namespace completions

namespace {

struct CompletionSite {
    lsp::Range replacementRange;
    const slang::parsing::Token* targetToken = nullptr;
    const slang::parsing::Token* tokenBefore = nullptr;
    const slang::parsing::Token* tokenAfter = nullptr;
    std::string typedPrefix;
};

CompletionSite getCompletionSite(const SlangDoc& doc, const ShallowAnalysis& analysis,
                                 slang::SourceLocation cursor) {
    using slang::parsing::TokenKind;

    // Probe the previous byte because a cursor at a token's end can fall outside token lookup.
    auto* targetToken = analysis.getWordTokenAt(cursor);
    if (!targetToken && cursor.offset() > 0)
        targetToken = analysis.getWordTokenAt(cursor - 1);

    // The fallback probe is valid only within the token or exactly at its end.
    if (targetToken &&
        (cursor < targetToken->range().start() || targetToken->range().end() < cursor)) {
        targetToken = nullptr;
    }

    // Replace the whole token so completion in its middle also removes the existing suffix.
    auto start = cursor;
    auto end = cursor;
    if (targetToken) {
        start = targetToken->range().start();
        end = targetToken->range().end();
    }

    auto* tokenBefore = analysis.syntaxes.getTokenBefore(start);
    if (!targetToken) {
        // Standalone `$` and backtick markers are not words but belong to the replacement range.
        auto* marker = analysis.syntaxes.getTokenBefore(cursor);
        if (marker && marker->range().end() == cursor &&
            (marker->kind == TokenKind::Dollar || marker->rawText() == "`")) {
            targetToken = marker;
            start = marker->range().start();
            end = marker->range().end();
            tokenBefore = analysis.syntaxes.getTokenBefore(start);
        }
    }

    // Filtering uses only text before the cursor even though replacement spans the whole token.
    std::string typedPrefix;
    if (targetToken) {
        auto length = std::min<size_t>(cursor.offset() - start.offset(),
                                       targetToken->rawText().size());
        typedPrefix = targetToken->rawText().substr(0, length);
    }

    // Neighboring tokens are measured outside the replacement range for query classification.
    return CompletionSite{
        .replacementRange = lsp::Range{.start = toPosition(start, doc.getSourceManager()),
                                       .end = toPosition(end, doc.getSourceManager())},
        .targetToken = targetToken,
        .tokenBefore = tokenBefore,
        .tokenAfter = analysis.syntaxes.getTokenAfter(end),
        .typedPrefix = std::move(typedPrefix),
    };
}

bool isSeparatedOnlyByWhitespace(const slang::parsing::Token& token) {
    return std::ranges::all_of(token.trivia(), [](const slang::parsing::Trivia& trivia) {
        return trivia.kind == slang::parsing::TriviaKind::Whitespace ||
               trivia.kind == slang::parsing::TriviaKind::EndOfLine;
    });
}

/// Items that name a module, interface, or class declaration, which can be instantiated
bool isInstantiableKind(lsp::CompletionItemKind kind) {
    return kind == lsp::CompletionItemKind::Module || kind == lsp::CompletionItemKind::Interface ||
           kind == lsp::CompletionItemKind::Class;
}

void setCompletionEdit(lsp::CompletionItem& item, const lsp::Range& replacementRange,
                       bool followedByCall, bool followedByInstantiation) {
    // Existing calls and instances still resolve documentation but retain their source shape.
    if ((followedByCall && item.kind == lsp::CompletionItemKind::Constant) ||
        (followedByInstantiation && item.kind && isInstantiableKind(*item.kind))) {
        item.insertText = item.label;
        item.insertTextFormat = lsp::InsertTextFormat::PlainText;
    }

    auto newText = item.insertText.value_or(item.label);
    auto useLabelOnly = (followedByCall &&
                         item.insertTextFormat == lsp::InsertTextFormat::Snippet) ||
                        (followedByInstantiation && item.kind && isInstantiableKind(*item.kind));
    if (useLabelOnly && item.insertTextFormat == lsp::InsertTextFormat::Snippet) {
        SnippetString escapedLabel;
        escapedLabel.appendText(item.label);
        newText = escapedLabel.getValue();
    }
    else if (useLabelOnly) {
        newText = item.label;
    }
    item.textEdit = lsp::TextEdit{.range = replacementRange, .newText = std::move(newText)};
}

} // namespace

namespace {

/// The instance whose port or parameter list the cursor is in, if any
struct InstanceListSite {
    const ast::InstanceSymbol* instance = nullptr;
    /// Set when the cursor is in the `#(...)` list of the instantiation
    const syntax::ParameterValueAssignmentSyntax* parameterList = nullptr;
    bool parameters = false;
    /// The connection or assignment the cursor is inside, which is still being typed
    const syntax::SyntaxNode* current = nullptr;
};

/// Walks the tokens around the cursor when the syntax tree has nothing to offer, which is what
/// happens for a `.` that has nothing after it yet: the parser cannot attach it to the instance.
std::optional<InstanceListSite> findInstanceListByTokens(const ShallowAnalysis& analysis,
                                                         slang::SourceLocation cursor) {
    auto* dot = analysis.syntaxes.getTokenBefore(cursor);
    if (!dot || dot->kind != parsing::TokenKind::Dot)
        return std::nullopt;

    // Walk back over the connections to the `(` that opens the list
    auto loc = dot->location();
    const parsing::Token* instanceName = nullptr;
    bool parameters = false;
    int depth = 0;
    for (int i = 0; i < 500; i++) {
        auto* token = analysis.syntaxes.getTokenBefore(loc);
        if (!token)
            return std::nullopt;
        loc = token->location();

        if (token->kind == parsing::TokenKind::CloseParenthesis) {
            depth++;
            continue;
        }
        if (token->kind != parsing::TokenKind::OpenParenthesis)
            continue;
        if (depth > 0) {
            depth--;
            continue;
        }

        // `u_inst (` opens the port list and `#(` opens the parameter list
        auto* before = analysis.syntaxes.getTokenBefore(token->location());
        if (before && before->kind == parsing::TokenKind::Hash)
            parameters = true;
        else
            instanceName = before;
        loc = token->range().end();
        break;
    }

    // The instance name comes after the parameter list, and before the port list
    for (int i = 0; !instanceName && i < 500; i++) {
        auto* token = analysis.syntaxes.getTokenAfter(loc);
        if (!token)
            return std::nullopt;
        loc = token->range().end();
        if (token->kind != parsing::TokenKind::Identifier)
            continue;

        auto* symbol = analysis.getSymbolAtToken(token);
        if (auto* instance = symbol ? symbol->as_if<ast::InstanceSymbol>() : nullptr)
            return InstanceListSite{instance, nullptr, parameters, nullptr};
    }

    if (!instanceName || instanceName->kind != parsing::TokenKind::Identifier)
        return std::nullopt;

    auto* symbol = analysis.getSymbolAtToken(instanceName);
    auto* instance = symbol ? symbol->as_if<ast::InstanceSymbol>() : nullptr;
    if (!instance)
        return std::nullopt;
    return InstanceListSite{instance, nullptr, false, nullptr};
}

std::optional<InstanceListSite> findInstanceListSite(const ShallowAnalysis& analysis,
                                                     slang::SourceLocation cursor) {
    const syntax::ParameterValueAssignmentSyntax* parameterList = nullptr;
    const syntax::SyntaxNode* current = nullptr;
    for (auto* node = analysis.syntaxes.getSyntaxAt(cursor); node; node = node->parent) {
        switch (node->kind) {
            // An expression inside a connection, or an ordered connection, is not a port name
            case syntax::SyntaxKind::OrderedPortConnection:
            case syntax::SyntaxKind::WildcardPortConnection:
                return std::nullopt;
            case syntax::SyntaxKind::NamedPortConnection: {
                auto& connection = node->as<syntax::NamedPortConnectionSyntax>();
                if (connection.openParen &&
                    cursor.offset() > connection.openParen.location().offset() &&
                    (!connection.closeParen ||
                     cursor.offset() <= connection.closeParen.location().offset())) {
                    return std::nullopt;
                }
                // Only the name the cursor is in is still being typed; a name that ends before the
                // cursor belongs to a connection that is already written
                if (connection.name && cursor.offset() <= connection.name.range().end().offset())
                    current = node;
                continue;
            }
            case syntax::SyntaxKind::NamedParamAssignment: {
                auto& assignment = node->as<syntax::NamedParamAssignmentSyntax>();
                if (assignment.openParen &&
                    cursor.offset() > assignment.openParen.location().offset() &&
                    (!assignment.closeParen ||
                     cursor.offset() <= assignment.closeParen.location().offset())) {
                    return std::nullopt;
                }
                if (assignment.name && cursor.offset() <= assignment.name.range().end().offset())
                    current = node;
                continue;
            }
            case syntax::SyntaxKind::ParameterValueAssignment:
                parameterList = &node->as<syntax::ParameterValueAssignmentSyntax>();
                continue;
            case syntax::SyntaxKind::HierarchicalInstance: {
                auto& instanceSyntax = node->as<syntax::HierarchicalInstanceSyntax>();
                if (!instanceSyntax.decl)
                    return std::nullopt;
                auto* symbol = analysis.getSymbolAtToken(&instanceSyntax.decl->name);
                auto* instance = symbol ? symbol->as_if<ast::InstanceSymbol>() : nullptr;
                if (!instance)
                    return std::nullopt;
                return InstanceListSite{instance, nullptr, false, current};
            }
            case syntax::SyntaxKind::HierarchyInstantiation: {
                // Reached only from the `#(...)` list, which is a sibling of the instances
                if (!parameterList)
                    return std::nullopt;
                auto& instantiation = node->as<syntax::HierarchyInstantiationSyntax>();
                if (instantiation.instances.empty() || !instantiation.instances[0]->decl)
                    return std::nullopt;
                auto* symbol = analysis.getSymbolAtToken(&instantiation.instances[0]->decl->name);
                auto* instance = symbol ? symbol->as_if<ast::InstanceSymbol>() : nullptr;
                if (!instance)
                    return std::nullopt;
                return InstanceListSite{instance, parameterList, true, current};
            }
            // Do not escape the declaration that contains this instantiation
            case syntax::SyntaxKind::ModuleDeclaration:
            case syntax::SyntaxKind::InterfaceDeclaration:
            case syntax::SyntaxKind::ProgramDeclaration:
            case syntax::SyntaxKind::GenerateBlock:
                return std::nullopt;
            default:
                continue;
        }
    }

    // The parser gives up on a connection that is still being typed
    return findInstanceListByTokens(analysis, cursor);
}

} // namespace

std::unique_ptr<CompletionQuery> CompletionQuery::fromLocation(
    const SlangDoc& doc, const std::shared_ptr<ShallowAnalysis>& analysis,
    slang::SourceLocation cursor, const lsp::CompletionContext& lspContext) {
    using slang::parsing::TokenKind;

    auto site = getCompletionSite(doc, *analysis, cursor);
    auto followedByCall = site.tokenAfter && site.tokenAfter->kind == TokenKind::OpenParenthesis &&
                          isSeparatedOnlyByWhitespace(*site.tokenAfter);
    auto followedByInstantiation = site.tokenAfter &&
                                   isSeparatedOnlyByWhitespace(*site.tokenAfter) &&
                                   (site.tokenAfter->kind == TokenKind::Identifier ||
                                    site.tokenAfter->kind == TokenKind::Hash);
    auto followedByColon = site.tokenAfter && site.tokenAfter->kind == TokenKind::Colon;

    // Inside `u_inst (...)` or `u_inst #(...)` the interesting names are the ports or parameters of
    // the instantiated module, which the generic completions do not know about. This has to be
    // checked before the `.` case below, which would look for a member of whatever precedes it.
    if (auto listSite = findInstanceListSite(*analysis, cursor); listSite && listSite->instance) {
        auto leadingDot = !(site.tokenBefore && site.tokenBefore->kind == TokenKind::Dot);
        return completions::InstancePortCompletionQuery::create(
            std::move(site.replacementRange), *listSite->instance, listSite->parameterList,
            listSite->parameters, listSite->current, leadingDot,
            completions::MemberCompletionQuery::createLexical(site.replacementRange, false, false));
    }

    if (site.targetToken && site.targetToken->rawText().starts_with('$')) {
        return completions::SystemSubroutineCompletionQuery::create(
            std::move(site.replacementRange), followedByCall);
    }
    if (site.targetToken && site.targetToken->rawText().starts_with('`')) {
        return completions::MacroCompletionQuery::create(std::move(site.replacementRange),
                                                         std::move(site.typedPrefix),
                                                         followedByCall);
    }
    if (site.tokenBefore && site.tokenBefore->kind == TokenKind::DoubleColon) {
        return completions::MemberCompletionQuery::createScopedAccess(
            std::move(site.replacementRange),
            analysis->syntaxes.getTokenBefore(site.tokenBefore->location()), followedByCall);
    }
    if (site.tokenBefore && site.tokenBefore->kind == TokenKind::Dot) {
        return completions::MemberCompletionQuery::createMemberAccess(
            std::move(site.replacementRange),
            analysis->syntaxes.getTokenBefore(site.tokenBefore->location()), followedByCall);
    }
    if (!site.targetToken && site.tokenBefore && site.tokenBefore->kind == TokenKind::Hash) {
        return completions::InstanceCompletionQuery::create(std::move(site.replacementRange),
                                                            analysis->syntaxes.getTokenBefore(
                                                                site.tokenBefore->location()));
    }
    if (analysis->getAssignmentPatternCompletionScope(cursor)) {
        if (lspContext.triggerKind == lsp::CompletionTriggerKind::TriggerCharacter &&
            lspContext.triggerCharacter == "{") {
            return completions::StructAssignCompletionQuery::create(
                std::move(site.replacementRange), cursor);
        }
        return completions::StructMemberCompletionQuery::create(std::move(site.replacementRange),
                                                                cursor, followedByColon);
    }

    return completions::MemberCompletionQuery::createLexical(std::move(site.replacementRange),
                                                             followedByCall,
                                                             followedByInstantiation);
}

void CompletionQuery::setCompletionEdit(lsp::CompletionItem& item) const {
    server::setCompletionEdit(item, replacementRange, followedByCall, followedByInstantiation);
}

ServerDriver& CompletionQuery::getDriver(CompletionDispatch& dispatch) {
    return dispatch.m_driver;
}

const Indexer& CompletionQuery::getIndexer(const CompletionDispatch& dispatch) {
    return dispatch.m_indexer;
}

slang::SourceManager& CompletionQuery::getSourceManager(CompletionDispatch& dispatch) {
    return dispatch.m_sourceManager;
}

slang::Bag& CompletionQuery::getOptions(CompletionDispatch& dispatch) {
    return dispatch.m_options;
}

bool CompletionQuery::resolvesCompletionEdits(const CompletionDispatch& dispatch) {
    return dispatch.resolveEdits;
}

void CompletionQuery::updateCompletionEditText(lsp::CompletionItem& item) {
    if (!item.insertText || !item.textEdit)
        return;

    if (rfl::holds_alternative<lsp::TextEdit>(*item.textEdit))
        rfl::get<lsp::TextEdit>(*item.textEdit).newText = *item.insertText;
    else
        rfl::get<lsp::InsertReplaceEdit>(*item.textEdit).newText = *item.insertText;
}

CompletionDispatch::CompletionDispatch(ServerDriver& driver, const Indexer& indexer,
                                       SourceManager& sourceManager, slang::Bag& options) :
    m_driver(driver), m_indexer(indexer), m_sourceManager(sourceManager), m_options(options) {
}

void CompletionDispatch::getCompletions(std::vector<lsp::CompletionItem>& results,
                                        std::shared_ptr<SlangDoc> doc,
                                        const CompletionContext& context) {
    SLANG_ASSERT(context.query);
    if (context.lspContext.triggerKind == lsp::CompletionTriggerKind::TriggerCharacter &&
        context.lspContext.triggerCharacter == "{" &&
        context.query->kind() != CompletionQueryKind::StructAssign) {
        return;
    }

    context.query->getCompletions(results, *this, doc, context);

    for (auto& item : results)
        context.query->setCompletionEdit(item);

    DEBUG("Returning {} completions for {} query in {} context", results.size(),
          toString(context.query->kind()), toString(context.kind));
}

void CompletionDispatch::getCompletionItemResolve(lsp::CompletionItem& item,
                                                  const lsp::RequestContext& ctx) {
    DEBUG("Resolving completion item: {}", item.label);
    ctx.throwIfCancelled("before resolving completion item");
    if (!item.label.empty() && item.label[0] == '$')
        return;

    // `kind` is optional in the protocol, and without it there is nothing to dispatch on
    if (!item.kind) {
        WARN("Completion item '{}' has no kind, nothing to resolve", item.label);
        return;
    }

    // Symbols that came out of the current analysis carry `data` and resolve against it; items
    // from the workspace index and the built-in tables have none.
    if (item.data) {
        completions::MemberCompletionQuery::resolve(*this, item, ctx);
        return;
    }

    switch (*item.kind) {
        case lsp::CompletionItemKind::Constant:
            completions::MacroCompletionQuery::resolve(*this, item);
            break;
        case lsp::CompletionItemKind::Module:
        case lsp::CompletionItemKind::Interface:
        case lsp::CompletionItemKind::Class:
            // Names of module, interface, and class declarations from the workspace index
            completions::InstanceCompletionQuery::resolve(*this, item);
            break;
        default:
            break;
    }
}

} // namespace server
