//------------------------------------------------------------------------------
// CompletionContext.cpp
// Syntax-aware completion context detection implementation
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/CompletionContext.h"

#include "document/SlangDoc.h"

#include "slang/ast/Scope.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/syntax/SyntaxFacts.h"
#include "slang/syntax/SyntaxKind.h"

namespace server {

using namespace slang;
using namespace slang::syntax;
using namespace slang::ast;

namespace {

/// Check if a syntax kind represents an expression (value context)
bool isExpressionContext(SyntaxKind kind) {
    if (SelectorSyntax::isKind(kind))
        return true;

    switch (kind) {
        // Port connections - values
        case SyntaxKind::OrderedPortConnection:
        case SyntaxKind::NamedPortConnection:
        // Function/task arguments
        case SyntaxKind::OrderedArgument:
        case SyntaxKind::NamedArgument:
        // Parameter assignments
        case SyntaxKind::OrderedParamAssignment:
        case SyntaxKind::NamedParamAssignment:
        // Conditional/control flow
        case SyntaxKind::ConditionalExpression:
        case SyntaxKind::ConditionalStatement:
        // Various expressions
        case SyntaxKind::ParenthesizedExpression:
        case SyntaxKind::InvocationExpression:
        case SyntaxKind::ExpressionStatement:

        // Value initializers
        case SyntaxKind::EqualsValueClause:
        // Macro arguments are unparsed source text from the caller. Treat them like expression
        // positions so value completions remain available inside macro usages.
        case SyntaxKind::MacroActualArgument:
        case SyntaxKind::MacroActualArgumentList:
            return true;
        default:
            return false;
    }
}

/// Check if a syntax kind represents a port list context (type position, no module instantiation)
bool isPortListContext(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::AnsiPortList:
        case SyntaxKind::NonAnsiPortList:
        case SyntaxKind::WildcardPortList:
        case SyntaxKind::ImplicitAnsiPort:
        case SyntaxKind::PortDeclaration:
            return true;
        default:
            return false;
    }
}

/// Check if a syntax kind represents a module/class item context
bool isModuleMemberContext(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::ModuleDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::ProgramDeclaration:
        case SyntaxKind::PackageDeclaration:
        case SyntaxKind::ClassDeclaration:
        case SyntaxKind::GenerateBlock:
        case SyntaxKind::GenerateRegion:
            return true;
        default:
            return false;
    }
}

/// Check if a syntax kind is part of a declaration, where the cursor is inside something that was
/// already written rather than at the start of a new item
bool isDeclarationContext(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::ParameterDeclaration:
        case SyntaxKind::TypeParameterDeclaration:
        case SyntaxKind::ParameterPortList:
        case SyntaxKind::ParameterValueAssignment:
            return true;
        default:
            return false;
    }
}

/// Check if a syntax kind declares a type and a name, where the cursor is inside the declaration
/// rather than at the start of a new item
bool isDeclaringMember(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::DataDeclaration:
        case SyntaxKind::NetDeclaration:
        case SyntaxKind::TypedefDeclaration:
        case SyntaxKind::PortDeclaration:
        case SyntaxKind::LocalVariableDeclaration:
        case SyntaxKind::SpecparamDeclaration:
            return true;
        default:
            return false;
    }
}

/// The module name of an instantiation is completed with module names even though the cursor is
/// inside an item that was already written
bool isInstantiationType(const SyntaxNode& node, SourceLocation loc) {
    if (node.kind != SyntaxKind::HierarchyInstantiation)
        return false;

    auto range = node.as<HierarchyInstantiationSyntax>().type.range();
    return loc >= range.start() && loc <= range.end();
}

/// Check if a syntax kind represents a procedural block context (task/function/always/initial)
bool isProceduralBlockContext(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::FunctionDeclaration:
        case SyntaxKind::TaskDeclaration:
        case SyntaxKind::AlwaysBlock:
        case SyntaxKind::AlwaysCombBlock:
        case SyntaxKind::AlwaysFFBlock:
        case SyntaxKind::AlwaysLatchBlock:
        case SyntaxKind::InitialBlock:
        case SyntaxKind::FinalBlock:
        case SyntaxKind::SequentialBlockStatement:
        case SyntaxKind::ParallelBlockStatement:
            return true;
        default:
            return false;
    }
}

} // anonymous namespace

CompletionContext CompletionContext::fromLocation(SlangDoc& doc, SourceLocation loc,
                                                  lsp::CompletionContext lspContext) {
    CompletionContext ctx;
    ctx.lspContext = std::move(lspContext);
    ctx.location = loc;
    // Hold analysis alive so that scope/syntax pointers remain valid
    // for the entire lifetime of the CompletionContext.
    ctx.analysis = doc.getAnalysis();
    ctx.scope = ctx.analysis->getScopeAt(loc);
    ctx.syntax = ctx.analysis->syntaxes.getSyntaxAt(loc);
    ctx.query = CompletionQuery::fromLocation(doc, ctx.analysis, loc, ctx.lspContext);

    if (!ctx.syntax) {
        // No syntax node at location - assume module item context if we have a scope
        ctx.kind = ctx.scope ? CompletionContextKind::ModuleMember : CompletionContextKind::Unknown;
        return ctx;
    }

    // Walk up the parent chain to determine context
    for (auto* node = ctx.syntax; node; node = node->parent) {
        auto kind = node->kind;

        // Check for expression contexts
        if (SyntaxFacts::isAssignmentOperator(kind) || isExpressionContext(kind)) {
            ctx.kind = CompletionContextKind::Expression;
            return ctx;
        }

        // Check for procedural block contexts (task/function/always/initial)
        if (isProceduralBlockContext(kind)) {
            ctx.kind = CompletionContextKind::Procedural;
            return ctx;
        }

        // Check for port list contexts - want types but not module instantiations
        if (isPortListContext(kind)) {
            ctx.kind = CompletionContextKind::PortList;
            return ctx;
        }

        // Check for module/class scope boundaries — if we reach one directly,
        // we're at the top level of the module body (declaration position).
        if (isModuleMemberContext(kind)) {
            ctx.kind = CompletionContextKind::ModuleMember;
            return ctx;
        }

        // A declaration the cursor is inside of is not a place where a new item can start, so
        // statement-level completions do not apply to it
        if (isDeclarationContext(kind)) {
            ctx.kind = CompletionContextKind::Declaration;
            return ctx;
        }

        // Any other member syntax (continuous assign, hierarchy instantiation, etc.)
        // that are not on the first token may need signals
        if (MemberSyntax::isKind(kind)) {
            // The first token of an item is the type it starts with: a module or interface to
            // instantiate, a type to declare, or the package of `pkg::name`. While that word is
            // still being written nothing about the item is decided — the parser only guesses when
            // it glues the word onto what follows — so the item is still starting here and the
            // scope it starts in is what belongs at the cursor. A keyword is a type that is
            // already settled, which makes the rest of the item a declaration that was written.
            auto firstToken = node->getFirstToken();
            if (!parsing::LexerFacts::isKeyword(firstToken.kind) &&
                firstToken.range().start() <= loc && loc <= firstToken.range().end()) {
                continue;
            }

            // Inside the item rather than after it: the cursor is in the middle of something that
            // was already written, which is only valid for the rest of that same item. A
            // declaration is where types go, and the module name of an instantiation is where
            // module names go, so those keep the completions that fit them.
            if (node->getLastToken().range().end() > loc && !isInstantiationType(*node, loc)) {
                ctx.kind = isDeclaringMember(kind) ? CompletionContextKind::Declaration
                                                   : CompletionContextKind::Expression;
                return ctx;
            }
            if (node->getFirstToken().range().end() < loc) {
                ctx.kind = CompletionContextKind::Expression;
                return ctx;
            }
        }
    }

    // Default to module item if we have a scope
    ctx.kind = ctx.scope ? CompletionContextKind::ModuleMember : CompletionContextKind::Unknown;
    return ctx;
}

} // namespace server
