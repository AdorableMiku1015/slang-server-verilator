//------------------------------------------------------------------------------
// InstanceCompletions.h
// Module, interface, package, and class completions.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "completions/CompletionContext.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/syntax/SyntaxTree.h"

namespace slang::parsing {
class Token;
}

namespace slang::syntax {
struct ParameterValueAssignmentSyntax;
class SyntaxNode;
} // namespace slang::syntax

namespace server::completions {

/// Completes the named ports of the instance whose connection list contains the cursor, or its
/// parameter overrides when the cursor is in the `#(...)` list. Already connected ports are left
/// out, and the insertion uses the `.port(port)` form that the surrounding code already uses.
class InstancePortCompletionQuery : public CompletionQuery {
public:
    /// @param instance the instantiated module or interface
    /// @param parameterList set to complete the `#(...)` list instead of the ports
    /// @param current the connection or assignment being typed, which is still offered
    /// @param leadingDot insert a `.` because the source does not have one yet
    /// @param afterSeparator the list or a comma is right before the cursor, so an insertion does
    ///        not need a separator of its own
    /// @param fallback general completions to append after the ports
    static std::unique_ptr<CompletionQuery> create(
        lsp::Range replacementRange, const slang::ast::InstanceSymbol& instance,
        const slang::syntax::ParameterValueAssignmentSyntax* parameterList, bool parameters,
        const slang::syntax::SyntaxNode* current, bool leadingDot, bool afterSeparator,
        std::unique_ptr<CompletionQuery> fallback);

protected:
    using CompletionQuery::CompletionQuery;
};

/// Query, candidate generation, and resolver for indexed design-unit completions.
class InstanceCompletionQuery : public CompletionQuery {
public:
    static std::unique_ptr<CompletionQuery> create(lsp::Range replacementRange,
                                                   const slang::parsing::Token* moduleToken);

    static void addCompletions(std::vector<lsp::CompletionItem>& results, const Indexer& indexer,
                               const CompletionContext& context);

    static lsp::CompletionItem getCompletion(std::string name, slang::syntax::SyntaxKind kind);

    static void resolve(const slang::syntax::SyntaxTree& tree, std::string_view moduleName,
                        lsp::CompletionItem& item, bool excludeName = false);

    static void resolve(CompletionDispatch& dispatch, lsp::CompletionItem& item,
                        std::optional<std::filesystem::path> modulePath = std::nullopt,
                        bool excludeName = false);

protected:
    using CompletionQuery::CompletionQuery;

private:
    static void resolveModuleInstance(const slang::syntax::ModuleHeaderSyntax& header,
                                      lsp::CompletionItem& item, bool excludeName);
};

} // namespace server::completions
