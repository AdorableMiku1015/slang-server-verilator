//------------------------------------------------------------------------------
// InstanceCompletions.cpp
// Module, interface, package, and class completions.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/InstanceCompletions.h"

#include "Indexer.h"
#include "completions/CompletionDispatch.h"
#include "completions/MemberCompletions.h"
#include "lsp/SnippetString.h"
#include "util/Converters.h"
#include "util/Formatting.h"
#include "util/Logging.h"
#include <fmt/format.h>
#include <unordered_set>

#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/TypePrinter.h"
#include "slang/parsing/Token.h"
#include "slang/parsing/TokenKind.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxVisitor.h"

namespace server::completions {
using namespace slang;

namespace {

class PortVisitor : public syntax::SyntaxVisitor<PortVisitor> {
public:
    std::vector<std::string_view> names;
    size_t maxLen = 0;

    void handle(const syntax::DeclaratorSyntax& port) {
        names.push_back(port.name.valueText());
        maxLen = std::max(maxLen, port.name.valueText().length());
    }

    void handle(const syntax::ExplicitNonAnsiPortSyntax& portDecl) {
        names.push_back(portDecl.name.valueText());
        maxLen = std::max(maxLen, portDecl.name.valueText().length());
    }
};

void collectParams(const syntax::ParameterPortListSyntax& paramList,
                   std::vector<std::string_view>& names, std::vector<std::string>& defaults,
                   size_t& maxLen) {
    bool lastLocal = false;
    for (auto declaration : paramList.declarations) {
        if (declaration->keyword)
            lastLocal = declaration->keyword.kind == parsing::TokenKind::LocalParamKeyword;
        if (lastLocal)
            continue;

        if (declaration->kind == syntax::SyntaxKind::ParameterDeclaration) {
            auto& paramSyntax = declaration->as<syntax::ParameterDeclarationSyntax>();
            for (auto decl : paramSyntax.declarators) {
                names.push_back(decl->name.valueText());
                std::string defaultVal = decl->initializer ? decl->initializer->expr->toString()
                                                           : "";
                ltrim(defaultVal);
                defaults.push_back(std::move(defaultVal));
                maxLen = std::max(maxLen, decl->name.valueText().length());
            }
        }
        else {
            auto& paramSyntax = declaration->as<syntax::TypeParameterDeclarationSyntax>();
            for (auto decl : paramSyntax.declarators) {
                names.push_back(decl->name.valueText());
                std::string defaultVal = decl->assignment ? decl->assignment->type->toString() : "";
                ltrim(defaultVal);
                defaults.push_back(std::move(defaultVal));
                maxLen = std::max(maxLen, decl->name.valueText().length());
            }
        }
    }
}

class InstanceCompletionQueryImpl final : public InstanceCompletionQuery {
public:
    InstanceCompletionQueryImpl(lsp::Range replacementRange, const parsing::Token* moduleToken) :
        InstanceCompletionQuery(std::move(replacementRange)), moduleToken(moduleToken) {}

    CompletionQueryKind kind() const final { return CompletionQueryKind::InstantiationSuffix; }

    void getCompletions(std::vector<lsp::CompletionItem>& results, CompletionDispatch& dispatch,
                        const std::shared_ptr<SlangDoc>&, const CompletionContext&) const final {
        if (!moduleToken) {
            WARN("No module token found before instantiation suffix");
            return;
        }

        auto name = moduleToken->valueText();
        auto symbolLoc = getIndexer(dispatch).getFirstSymbolLoc(name);
        if (!symbolLoc) {
            ERROR("No module found for {}", name);
            return;
        }

        auto completion = getCompletion(std::string(name), symbolLoc->kind);
        resolve(dispatch, completion, *symbolLoc->uri, true);
        results.push_back(std::move(completion));
    }

private:
    const parsing::Token* moduleToken;
};

} // namespace

std::unique_ptr<CompletionQuery> InstanceCompletionQuery::create(
    lsp::Range replacementRange, const parsing::Token* moduleToken) {
    return std::make_unique<InstanceCompletionQueryImpl>(std::move(replacementRange), moduleToken);
}

lsp::CompletionItem InstanceCompletionQuery::getCompletion(std::string name,
                                                           syntax::SyntaxKind kind) {
    std::string detail;
    switch (kind) {
        case syntax::SyntaxKind::ModuleDeclaration:
            detail = " Module";
            break;
        case syntax::SyntaxKind::InterfaceDeclaration:
            detail = " Interface";
            break;
        default:
            detail = std::string(toString(kind));
            break;
    }

    return lsp::CompletionItem{
        .label = name,
        .labelDetails =
            lsp::CompletionItemLabelDetails{
                .detail = detail,
            },
        .kind = lsp::CompletionItemKind::Module,
        .filterText = name,
    };
}

void InstanceCompletionQuery::addCompletions(std::vector<lsp::CompletionItem>& results,
                                             const Indexer& indexer,
                                             const CompletionContext& context) {
    std::unordered_set<std::string_view> seenNames;
    auto first = results.size();

    indexer.forEachSymbol([&](const std::string& name, const Indexer::GlobalSymbolLoc& entry) {
        if (!seenNames.insert(name).second)
            return;

        std::string detail;
        std::optional<std::string> insertText;
        lsp::CompletionItemKind kind = lsp::CompletionItemKind::Module;
        switch (entry.kind) {
            case syntax::SyntaxKind::ModuleDeclaration:
                if (context.kind != CompletionContextKind::ModuleMember)
                    return;
                detail = " Module";
                break;
            case syntax::SyntaxKind::InterfaceDeclaration:
                kind = lsp::CompletionItemKind::Interface;
                detail = " Interface";
                if (context.kind != CompletionContextKind::ModuleMember)
                    insertText = name;
                break;
            case syntax::SyntaxKind::PackageDeclaration:
                // A package is only usable as a scope, so it is noise in an expression
                if (context.kind == CompletionContextKind::Expression ||
                    context.kind == CompletionContextKind::Procedural) {
                    return;
                }
                detail = " Package";
                break;
            case syntax::SyntaxKind::ClassDeclaration:
                if (context.kind == CompletionContextKind::Expression)
                    return;
                kind = lsp::CompletionItemKind::Class;
                detail = " Class";
                break;
            default:
                return;
        }
        results.push_back(lsp::CompletionItem{
            .label = name,
            .labelDetails =
                lsp::CompletionItemLabelDetails{
                    .detail = detail,
                },
            .kind = kind,
            .filterText = name,
            .insertText = insertText,
        });
    });

    rankCompletions(results, first, rank::Library);
}

void InstanceCompletionQuery::resolveModuleInstance(const syntax::ModuleHeaderSyntax& header,
                                                    lsp::CompletionItem& item, bool excludeName) {
    item.documentation = svCodeBlock(header);
    if (item.insertText)
        return;

    SnippetString output;
    size_t maxLen = 0;
    std::vector<std::string_view> names;
    std::vector<std::string> defaults;
    if (header.parameters)
        collectParams(*header.parameters, names, defaults, maxLen);

    if (!excludeName)
        output.appendText(header.name.valueText());

    if (!names.empty()) {
        if (!excludeName)
            output.appendText(" #");
        output.appendText("(\n");

        for (size_t i = 0; i < names.size(); ++i) {
            auto name = std::string(names[i]);
            auto nameFmt = name + std::string(maxLen - name.length(), ' ');
            output.appendText("\t." + nameFmt + "(");
            if (defaults[i].empty())
                output.appendPlaceholder(name);
            else
                output.appendPlaceholder(fmt::format("{} /* default {} */", name, defaults[i]));
            output.appendText(")");
            output.appendText(i < names.size() - 1 ? ",\n" : "\n ");
        }
        output.appendText(")");
    }

    output.appendText(" ");
    output.appendPlaceholder(toCamelCase(header.name.valueText()));
    output.appendText(" (");

    maxLen = 0;
    names.clear();
    if (header.ports) {
        PortVisitor visitor;
        header.ports->visit(visitor);
        names = std::move(visitor.names);
        maxLen = visitor.maxLen;
    }

    if (!names.empty()) {
        output.appendText("\n");
        for (size_t i = 0; i < names.size(); ++i) {
            auto name = std::string(names[i]);
            auto nameFmt = name + std::string(maxLen - name.length(), ' ');
            output.appendText("\t." + nameFmt + "(");
            output.appendPlaceholder(name);
            output.appendText(")");
            output.appendText(i < names.size() - 1 ? ",\n" : "\n");
        }
    }

    output.appendText(");");
    item.insertText = output.getValue();
    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
}

void InstanceCompletionQuery::resolve(const syntax::SyntaxTree& tree, std::string_view moduleName,
                                      lsp::CompletionItem& item, bool excludeName) {
    for (auto [module, node] : tree.getMetadata().nodeMeta) {
        auto& header = *module->header;
        if (header.name.valueText() != moduleName)
            continue;

        switch (module->kind) {
            case syntax::SyntaxKind::InterfaceDeclaration:
            case syntax::SyntaxKind::ModuleDeclaration:
                resolveModuleInstance(header, item, excludeName);
                break;
            default:
                item.documentation = svCodeBlock(header);
                item.insertText = header.name.valueText();
                item.insertTextFormat = lsp::InsertTextFormat::PlainText;
                continue;
        }
        break;
    }
}

void InstanceCompletionQuery::resolve(CompletionDispatch& dispatch, lsp::CompletionItem& item,
                                      std::optional<std::filesystem::path> modulePath,
                                      bool excludeName) {
    auto name = item.label;
    if (!modulePath) {
        auto files = getIndexer(dispatch).getFilesForSymbol(name);
        if (files.empty()) {
            WARN("No files found for module {}", name);
            return;
        }
        if (files.size() > 1)
            WARN("Multiple files found for module {}: {}", name, rfl::json::write(files));
        modulePath = files[0];
    }

    auto maybeTree = syntax::SyntaxTree::fromFile(modulePath->string(), getSourceManager(dispatch),
                                                  getOptions(dispatch));
    if (!maybeTree) {
        WARN("Failed to load syntax tree for module {} from {}", name, modulePath->string());
        return;
    }

    resolve(*maybeTree.value(), name, item, excludeName);
    updateCompletionEditText(item);
}

namespace {

/// Type detail for a port or parameter, like " logic[31:0]"
std::string getTypeDetailString(const ast::Type& type) {
    ast::TypePrinter printer;
    printer.options.elideScopeNames = true;
    printer.options.skipTypeDefs = true;
    printer.append(type);
    return printer.toString();
}

/// The `.port(port)` insertion, with the expression as a placeholder so the cursor lands in it.
/// The implicit `.port` form is used when the source already names the port, which is what the
/// surrounding code does.
std::string getConnectionSnippet(std::string_view name, bool hasExpression) {
    SnippetString output;
    output.appendText("." + std::string(name) + "(");
    if (hasExpression)
        output.appendPlaceholder(std::string(name));
    else
        output.appendPlaceholder("");
    output.appendText("),");
    return std::string(output.getValue());
}

class InstancePortCompletionQueryImpl final : public InstancePortCompletionQuery {
public:
    InstancePortCompletionQueryImpl(lsp::Range replacementRange,
                                    const ast::InstanceSymbol& instance,
                                    const syntax::ParameterValueAssignmentSyntax* parameterList,
                                    bool parameters, const syntax::SyntaxNode* current,
                                    bool leadingDot, bool afterSeparator,
                                    std::unique_ptr<CompletionQuery> fallback) :
        InstancePortCompletionQuery(std::move(replacementRange)), instance(instance),
        parameterList(parameterList), parameters(parameters), current(current),
        leadingDot(leadingDot), afterSeparator(afterSeparator), fallback(std::move(fallback)) {}

    CompletionQueryKind kind() const final { return CompletionQueryKind::InstancePorts; }

    void getCompletions(std::vector<lsp::CompletionItem>& results, CompletionDispatch& dispatch,
                        const std::shared_ptr<SlangDoc>& doc,
                        const CompletionContext& context) const final {
        auto first = results.size();
        auto documentUri = doc->getURI().str();
        if (parameters)
            addParameters(results, context, documentUri);
        else
            addPorts(results, context, documentUri);
        DEBUG("Returning {} {} completions for {}", results.size() - first,
              parameters ? "parameter" : "port", instance.body.getDefinition().name);
        rankCompletions(results, first, rank::Scope);

        if (generalCompletionsFit()) {
            auto fallbackFirst = results.size();
            fallback->getCompletions(results, dispatch, doc, context);

            // The port items are the ones that fit the position, so a symbol that is also a port
            // of this instance does not need to appear twice
            std::unordered_set<std::string> seen;
            for (auto it = results.begin(); it != results.begin() + fallbackFirst; ++it)
                seen.insert(it->label);
            results.erase(std::remove_if(results.begin() + fallbackFirst, results.end(),
                                         [&](const lsp::CompletionItem& item) {
                                             return !seen.insert(item.label).second;
                                         }),
                          results.end());
        }
    }

private:
    /// Whether the general completions fit where the cursor is: a named list only takes
    /// `.name(...)` connections, and anything that follows a connection needs a separator first. A
    /// `.` that was just typed is a position of its own, where the implicit `.name` form of a port
    /// is valid.
    bool generalCompletionsFit() const {
        if (!fallback)
            return false;
        if (!leadingDot)
            return !parameters;
        return listIsEmpty() || afterSeparator;
    }

    /// Whether the list has nothing in it yet, which is the only place a positional connection or
    /// assignment can still be added
    bool listIsEmpty() const {
        if (parameters) {
            return !parameterList || parameterList->parameters.empty();
        }
        if (auto* syntax = instance.getSyntax();
            syntax && syntax->kind == syntax::SyntaxKind::HierarchicalInstance) {
            return syntax->as<syntax::HierarchicalInstanceSyntax>().connections.empty();
        }
        return true;
    }
    /// The name of the connection or assignment the cursor is in, which is still being typed and
    /// must stay in the list even though the analysis already sees it as connected
    std::string_view currentName() const {
        if (!current)
            return {};
        if (current->kind == syntax::SyntaxKind::NamedPortConnection)
            return current->as<syntax::NamedPortConnectionSyntax>().name.valueText();
        if (current->kind == syntax::SyntaxKind::NamedParamAssignment)
            return current->as<syntax::NamedParamAssignmentSyntax>().name.valueText();
        return {};
    }

    void addPorts(std::vector<lsp::CompletionItem>& results, const CompletionContext& context,
                  std::string_view documentUri) const {
        // Which ports the source connects. The analysis has a connection for every port, including
        // the ones that are not connected at all, so the syntax is what tells them apart.
        auto typing = currentName();
        std::unordered_set<std::string_view> connected;
        if (auto* syntax = instance.getSyntax();
            syntax && syntax->kind == syntax::SyntaxKind::HierarchicalInstance) {
            auto& instanceSyntax = syntax->as<syntax::HierarchicalInstanceSyntax>();
            for (auto* connection : instanceSyntax.connections) {
                auto* named = connection ? connection->as_if<syntax::NamedPortConnectionSyntax>()
                                         : nullptr;
                if (named && named->name.valueText() != typing)
                    connected.insert(named->name.valueText());
            }
        }

        for (auto* symbol : instance.body.getPortList()) {
            if (!symbol || symbol->name.empty() || connected.contains(symbol->name))
                continue;
            if (auto* multi = symbol->as_if<ast::MultiPortSymbol>()) {
                for (auto* port : multi->ports) {
                    if (!connected.contains(port->name))
                        addPort(results, *port, context, documentUri);
                }
                continue;
            }
            addPort(results, *symbol, context, documentUri);
        }
    }

    void addPort(std::vector<lsp::CompletionItem>& results, const ast::Symbol& port,
                 const CompletionContext& context, std::string_view documentUri) const {
        auto* portSymbol = port.as_if<ast::PortSymbol>();
        auto* ifacePort = port.as_if<ast::InterfacePortSymbol>();
        if (!portSymbol && !ifacePort)
            return;

        std::string detail;
        if (ifacePort) {
            detail = " interface";
            if (ifacePort->interfaceDef)
                detail += " " + std::string(ifacePort->interfaceDef->name);
        }
        else {
            detail = " " + portString(portSymbol->direction) + " " +
                     getTypeDetailString(portSymbol->getType());
        }

        results.push_back(getConnectionItem(port, port.name, detail, context, documentUri));
    }

    void addParameters(std::vector<lsp::CompletionItem>& results, const CompletionContext& context,
                       std::string_view documentUri) const {
        // Parameters that already have a value in the `#(...)` list are done
        auto typing = currentName();
        std::unordered_set<std::string_view> assigned;
        for (auto* assignment :
             parameterList ? parameterList->parameters
                           : syntax::SeparatedSyntaxList<syntax::ParamAssignmentSyntax>{}) {
            if (!assignment)
                continue;
            if (auto* named = assignment->as_if<syntax::NamedParamAssignmentSyntax>()) {
                if (named->name.valueText() != typing)
                    assigned.insert(named->name.valueText());
            }
        }

        for (auto* param : instance.body.getParameters()) {
            if (!param || param->symbol.name.empty() || param->isLocalParam() ||
                assigned.contains(param->symbol.name)) {
                continue;
            }

            std::string detail;
            if (auto* value = param->symbol.as_if<ast::ValueSymbol>())
                detail = " " + getTypeDetailString(value->getType());
            else
                detail = " type";

            results.push_back(
                getConnectionItem(param->symbol, param->symbol.name, detail, context, documentUri));
        }
    }

    lsp::CompletionItem getConnectionItem(const ast::Symbol& symbol, std::string_view name,
                                          std::string_view detail, const CompletionContext& context,
                                          std::string_view documentUri) const {
        // The implicit `.name` form needs a symbol with that name in scope, so only offer to
        // repeat the name when there is one
        auto hasExpression = context.scope ? context.scope->find(name) != nullptr : false;

        auto snippet = getConnectionSnippet(name, hasExpression);
        if (!leadingDot)
            snippet.erase(0, 1);
        // Without a separator of its own, the connection before this one would run into it
        if (leadingDot && !afterSeparator)
            snippet.insert(0, ", ");

        // Built like any other symbol item so that resolving it fills in the documentation
        auto item = MemberCompletionQuery::getSymbolCompletion(symbol, context.scope, documentUri);
        item.labelDetails = lsp::CompletionItemLabelDetails{
            .detail = std::string(detail),
        };
        item.filterText = std::string(name);
        item.insertText = std::move(snippet);
        item.insertTextFormat = lsp::InsertTextFormat::Snippet;
        item.sortText = std::string(rank::Scope);
        return item;
    }

    const ast::InstanceSymbol& instance;
    const syntax::ParameterValueAssignmentSyntax* parameterList;
    bool parameters;
    const syntax::SyntaxNode* current;
    bool leadingDot;
    bool afterSeparator;
    std::unique_ptr<CompletionQuery> fallback;
};

} // namespace

std::unique_ptr<CompletionQuery> InstancePortCompletionQuery::create(
    lsp::Range replacementRange, const ast::InstanceSymbol& instance,
    const syntax::ParameterValueAssignmentSyntax* parameterList, bool parameters,
    const syntax::SyntaxNode* current, bool leadingDot, bool afterSeparator,
    std::unique_ptr<CompletionQuery> fallback) {
    return std::make_unique<InstancePortCompletionQueryImpl>(std::move(replacementRange), instance,
                                                             parameterList, parameters, current,
                                                             leadingDot, afterSeparator,
                                                             std::move(fallback));
}

} // namespace server::completions
