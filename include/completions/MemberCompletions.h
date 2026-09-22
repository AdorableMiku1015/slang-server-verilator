//------------------------------------------------------------------------------
// MemberCompletions.h
// Lexical, member-access, and scoped-access completions.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "completions/CompletionContext.h"
#include "lsp/LspTypes.h"
#include "lsp/RequestContext.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "slang/ast/Symbol.h"

namespace slang::parsing {
class Token;
}

namespace server::completions {

/// Base for queries that produce AST member candidates and resolve their deferred data.
class MemberCompletionQuery : public CompletionQuery {
public:
    static std::unique_ptr<CompletionQuery> createLexical(lsp::Range replacementRange,
                                                          bool followedByCall,
                                                          bool followedByInstantiation);

    static std::unique_ptr<CompletionQuery> createMemberAccess(
        lsp::Range replacementRange, const slang::parsing::Token* receiverToken,
        bool followedByCall);

    static std::unique_ptr<CompletionQuery> createScopedAccess(
        lsp::Range replacementRange, const slang::parsing::Token* receiverToken,
        bool followedByCall);

    /// Resolve a member completion received through completionItem/resolve.
    static void resolve(CompletionDispatch& dispatch, lsp::CompletionItem& item,
                        const lsp::RequestContext& ctx);

    /// Populate deferred member properties while the symbol is already available.
    static void resolve(const slang::ast::Symbol& symbol, lsp::CompletionItem& item,
                        bool resolveCallableEdit = false);

    /// Build a completion item for a symbol with `data` left for `completionItem/resolve` to fill
    /// in the deferred parts (documentation, callable edit)
    static lsp::CompletionItem getSymbolCompletion(const slang::ast::Symbol& symbol,
                                                   const slang::ast::Scope* currentScope,
                                                   std::string_view documentUri);

protected:
    using CompletionQuery::CompletionQuery;

    /// @param seenLabels names already offered, so that a shadowed symbol is only listed once.
    ///        Wildcard imports share the set of their caller, whose members are nearer.
    static void addCompletions(std::vector<lsp::CompletionItem>& results,
                               const slang::ast::Scope* scope, CompletionContextKind contextKind,
                               const slang::ast::Scope* originalScope, std::string_view documentUri,
                               const CompletionContext& context, bool labelOnly = false,
                               bool deferCallableEdit = false, bool isOriginalCall = true,
                               std::unordered_set<std::string>* seenLabels = nullptr);

    static lsp::CompletionItem getHierarchicalCompletion(const slang::ast::Symbol& parentSymbol,
                                                         const slang::ast::Symbol& symbol,
                                                         std::string_view documentUri,
                                                         bool labelOnly = false,
                                                         bool deferCallableEdit = false,
                                                         std::string_view completionLabel = {});

private:
    struct CompletionData {
        std::string documentUri;
        std::string symbolPath;
        std::string symbolName;
        uint32_t bufferId = 0;
        uint64_t offset = 0;
        slang::ast::SymbolKind symbolKind = slang::ast::SymbolKind::Unknown;
        bool labelOnly = false;
    };

    static lsp::CompletionItemKind getCompletionKind(const slang::ast::Symbol& symbol);

    /// Offers the values of the enum being assigned to at the cursor
    static void addExpectedEnumCompletions(std::vector<lsp::CompletionItem>& results,
                                           const CompletionContext& context,
                                           const slang::ast::Scope* originalScope,
                                           std::string_view documentUri,
                                           std::unordered_set<std::string>* seen);
    static lsp::CompletionItem getCompletion(const slang::ast::Symbol& symbol,
                                             const slang::ast::Scope* currentScope,
                                             std::string_view documentUri, bool labelOnly,
                                             bool deferCallableEdit);
};

} // namespace server::completions
