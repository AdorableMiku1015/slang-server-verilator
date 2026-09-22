//------------------------------------------------------------------------------
// CompletionDispatch.h
// Dispatch controller for LSP completion requests and responses
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include "Indexer.h"
#include "completions/CompletionContext.h"
#include "document/SlangDoc.h"
#include "lsp/LspTypes.h"
#include "lsp/RequestContext.h"
#include <memory>
#include <string>
#include <vector>

#include "slang/text/SourceLocation.h"
#include "slang/util/Bag.h"

namespace server {

class ServerDriver;

class CompletionDispatch {
private:
    friend class CompletionQuery;

    // May need to retrieve additional documents
    ServerDriver& m_driver;
    const Indexer& m_indexer;
    SourceManager& m_sourceManager;
    slang::Bag& m_options;

public:
    bool resolveEdits = false;

    CompletionDispatch(ServerDriver& driver, const Indexer& indexer, SourceManager& sourceManager,
                       slang::Bag& options);

    /// Top-level completion entry point. The semantic target is derived from source around the
    /// cursor; trigger characters only control when clients invoke this method.
    void getCompletions(std::vector<lsp::CompletionItem>& results, std::shared_ptr<SlangDoc> doc,
                        const CompletionContext& ctx);

    void getCompletionItemResolve(lsp::CompletionItem& item, const lsp::RequestContext& ctx);
};

namespace completions {

/// Characters that clients should use to trigger completion requests.
const std::vector<std::string>& completionTriggerCharacters();

/// Completion items are ranked in layers, so that the list starts with what the cursor can
/// actually refer to. Clients sort by `sortText` before anything else, and fall back to their own
/// fuzzy matching within a layer, which keeps the good matches on top for any typed prefix.
namespace rank {
/// Symbols visible from the cursor: locals, ports, parameters, instances, types
constexpr std::string_view Scope = "0";
/// Members of packages the file imports
constexpr std::string_view Imported = "1";
/// Language keywords and snippets
constexpr std::string_view Keyword = "2";
/// Modules, interfaces, and classes anywhere in the workspace
constexpr std::string_view Library = "3";
} // namespace rank

/// Applies `rank` to every item added since `firstIndex`
inline void rankCompletions(std::vector<lsp::CompletionItem>& results, size_t firstIndex,
                            std::string_view rank) {
    for (size_t i = firstIndex; i < results.size(); i++)
        results[i].sortText = std::string(rank);
}

/// Applies `rank` to the items added since `firstIndex` that have no rank yet, so that items
/// ranked more specifically (an import, for example) keep their own layer.
inline void rankUnrankedCompletions(std::vector<lsp::CompletionItem>& results, size_t firstIndex,
                                    std::string_view rank) {
    for (size_t i = firstIndex; i < results.size(); i++) {
        if (!results[i].sortText)
            results[i].sortText = std::string(rank);
    }
}

} // namespace completions

} // namespace server
