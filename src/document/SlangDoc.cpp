//------------------------------------------------------------------------------
// SlangDoc.cpp
// Implementation of SlangDoc class
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "document/SlangDoc.h"

#include "ServerDriver.h"
#include "document/ShallowAnalysis.h"
#include "lsp/RequestContext.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <string>
#include <string_view>

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/CompilationDiags.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/String.h"
namespace server {

using namespace slang;

SlangDoc::SlangDoc(ServerDriver& driver, URI uri, SourceBuffer buffer) :
    m_driver(driver), m_sourceManager(driver.sm), m_options(driver.options), m_uri(uri),
    m_wsRelativePathOffset(driver.getWsRelativePathOffset(m_uri.getPath())), m_buffer(buffer) {
}

std::optional<SourceLocation> SlangDoc::getLocation(const lsp::Position& position) {
    getSyntaxTree();
    return toSourceLocation(m_buffer.id, position, m_sourceManager);
}

std::shared_ptr<SlangDoc> SlangDoc::fromTree(ServerDriver& driver,
                                             std::shared_ptr<syntax::SyntaxTree> tree) {
    auto buffer = SourceBuffer{
        .data = driver.sm.getSourceText(tree->getSourceBufferIds()[0]),
        .id = tree->getSourceBufferIds()[0],
    };
    auto uri = URI::fromFile(driver.sm.getFullPath(tree->getSourceBufferIds()[0]));
    auto ret = std::make_shared<SlangDoc>(driver, uri, buffer);
    ret->m_tree = tree;
    return ret;
}

std::shared_ptr<SlangDoc> SlangDoc::fromText(ServerDriver& driver, const URI& uri,
                                             std::string_view text) {
    auto path = std::filesystem::weakly_canonical(uri.getPath()).string();
    SourceBuffer buffer;

    // Check if this path was previously cached (e.g., from an include)
    // If so, we need to replace the old buffer with the editor's version
    if (driver.sm.isCached(path)) {
        auto existingBuffer = driver.sm.readSource(path, nullptr).value();
        if (existingBuffer.data.size() == text.size() + 1 &&
            existingBuffer.data.substr(0, text.size()) == text) {
            return std::make_shared<SlangDoc>(driver, uri, existingBuffer);
        }
        SmallVector<char> newBuffer;
        newBuffer.insert(newBuffer.end(), text.begin(), text.end());
        if (newBuffer.empty() || newBuffer.back() != '\0')
            newBuffer.push_back('\0');
        buffer = driver.sm.replaceBuffer(existingBuffer.id, std::move(newBuffer));
    }
    else {
        // assignText preserves its path verbatim; use the same cache key as readSource
        // so dependency invalidation cannot replace editor text with disk contents.
        buffer = driver.sm.assignText(getU8Str(std::filesystem::weakly_canonical(path)), text);
    }

    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

std::shared_ptr<SlangDoc> SlangDoc::open(ServerDriver& driver, const URI& uri) {
    auto buffer = driver.sm.readSource(uri.getPath(), nullptr).value();
    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

const std::string_view SlangDoc::getText() const {
    // null terminator is included in data
    return m_sourceManager.getSourceText(m_buffer.id);
}

bool SlangDoc::isMacroOnly() {
    auto buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
    if (m_macroOnlyBuffer != buffer.id) {
        auto options = m_driver.options;
        auto ppOptions = options.getOrDefault<parsing::PreprocessorOptions>();
        ppOptions.maxIncludeDepth = 0;
        options.set(ppOptions);
        auto tree = syntax::SyntaxTree::fromBuffer(buffer, m_sourceManager, options);
        m_macroOnly = tree->root().as<syntax::CompilationUnitSyntax>().members.empty() &&
                      tree->getIncludeDirectives().empty() && tree->diagnostics().empty() &&
                      std::ranges::any_of(tree->getDefinedMacros(), [&](const auto* macro) {
                          return macro->name.location().buffer() == buffer.id;
                      });
        m_macroOnlyBuffer = buffer.id;
    }
    return m_macroOnly;
}

std::shared_ptr<syntax::SyntaxTree> SlangDoc::getSyntaxTree() {
    const auto& path = m_sourceManager.getFullPath(m_buffer.id);
    if (auto contexts = m_driver.getIncludeContexts(path, true); !contexts.empty()) {
        const auto& context = contexts.front();
        auto compilation = context.source->getAnalysis()->getShallowCompilation();
        auto tree = compilation->getSyntaxTree();
        if (m_includeCompilation != compilation || m_tree != tree ||
            m_buffer.id != context.buffer) {
            m_analysis.reset();
            m_tree = tree;
            m_buffer = SourceBuffer{.data = m_sourceManager.getSourceText(context.buffer),
                                    .library = m_sourceManager.getLibraryFor(context.buffer),
                                    .id = context.buffer};
            m_options = tree->options();
            m_includeCompilation = std::move(compilation);
        }
        return m_tree;
    }
    if (m_includeCompilation) {
        m_includeCompilation.reset();
        m_tree.reset();
        m_analysis.reset();
    }
    if (!m_tree) {
        // Will read the cached file data if it exists
        if (!m_sourceManager.isLatestData(m_buffer.id)) {
            m_buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
        }
        m_tree = m_driver.parseShallowTree(m_buffer, m_options);
    }
    else if (!hasValidBuffers(m_sourceManager, m_tree)) {
        // Tree has invalid buffers, need to reparse
        m_buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
        m_tree = m_driver.parseShallowTree(m_buffer, m_options);
    }
    return m_tree;
}

std::shared_ptr<ShallowAnalysis> SlangDoc::refreshAnalysis(const lsp::RequestContext& ctx) {
    ctx.throwIfCancelled("before analysis");
    auto tree = getSyntaxTree();
    auto compilation = m_includeCompilation;
    if (!compilation) {
        auto dependencies = m_driver.getDependentTrees(tree);
        compilation = std::make_shared<ShallowCompilation>(m_sourceManager, tree, m_options,
                                                           dependencies, m_driver.comp.get());
    }
    auto analysis = std::make_shared<ShallowAnalysis>(m_buffer.id, std::move(compilation));
    auto topNames = analysis->getCompilation()->getRoot().topInstances |
                    std::views::transform([](const auto& top) { return top->name; });
    ctx.debug("Analyzed {} with tops: {}", getWsRelativePath(), fmt::join(topNames, ", "));
    m_analysis = analysis;
    return analysis;
}

std::shared_ptr<ShallowAnalysis> SlangDoc::getAnalysis(const lsp::RequestContext& ctx) {
    getSyntaxTree();
    if (!m_analysis || !m_analysis->hasValidBuffers())
        return refreshAnalysis(ctx);
    return m_analysis;
}

bool SlangDoc::textMatches(std::string_view text) {
    // Just compute line offsets to validate UTF-8
    auto bufText = getText();
    if (bufText.size() != text.size() + 1) {
        ERROR("Text size mismatch: have {}, expected {}", bufText.size(), text.size() + 1);
        return false;
    }
    if (std::memcmp(bufText.data(), text.data(), bufText.size()) != 0) {
        ERROR("Text content mismatch");
        return false;
    }
    return true;
}

void SlangDoc::onChange(const std::vector<lsp::TextDocumentContentChangeEvent>& contentChanges) {
    // From the LSP spec:
    //
    // The actual content changes. The content changes describe single state changes to the
    // document. So if there are two content changes c1 (at array index 0) and c2 (at array
    // index 1) for a document in state S then c1 moves the document from S to S' and c2 from
    // S' to S''. So c1 is computed on the state S and c2 is computed on the state S'.
    //
    // To mirror the content of a document using change events use the following approach:
    // - start with the same initial content
    // - apply the 'textDocument/didChange' notifications in the order you receive them.
    // - apply the `TextDocumentContentChangeEvent`s in a single notification in the order you
    //   receive them.
    //
    // Partial is defined in the variant first, so it will match first iff range and text are both
    // present. WholeDocument will match if text is present, but only be tried second. Less specific
    // cases are tried later.
    SmallVector<char> buffer;
    std::string_view textView = getText();
    std::vector<size_t> lineOffsets;

    if (contentChanges.size() == 0) {
        ERROR("Empty onChange event");
        return;
    }

    // LSP Position.character is a UTF-16 code unit: this server answers with no
    // positionEncoding, which leaves the client on the UTF-16 default it already speaks
    auto getOffsets = [&](lsp::Range range) {
        // Only one thread is able to call onchange, so the offsets remain valid without locking
        SourceManager::computeLineOffsets(textView, lineOffsets);
        // LSP positions point at a line and a UTF-16 column, but the buffer is UTF-8 bytes.
        auto getOffset = [&](const lsp::Position& pos, std::string_view which) {
            if (pos.line >= lineOffsets.size()) {
                throw std::runtime_error(fmt::format("{} line out of bounds: {} / {}", which,
                                                     pos.line, lineOffsets.size()));
            }

            auto lineStart = lineOffsets[pos.line];
            auto lineEnd = pos.line + 1 < lineOffsets.size() ? lineOffsets[pos.line + 1]
                                                             : textView.size();
            // Line offsets point past the line terminator, which is not part of the line
            while (lineEnd > lineStart &&
                   (textView[lineEnd - 1] == '\n' || textView[lineEnd - 1] == '\r')) {
                lineEnd--;
            }

            // Rejecting bad positions keeps a stale client from corrupting the buffer
            auto column = utf16ToByteOffset(textView.substr(lineStart, lineEnd - lineStart),
                                            pos.character);
            if (!column) {
                throw std::runtime_error(fmt::format("{} column out of bounds: {} on line {}",
                                                     which, pos.character, pos.line));
            }
            return lineStart + *column;
        };
        auto offsets = std::make_pair(getOffset(range.start, "Start"), getOffset(range.end, "End"));
        // A reversed range, or one that reaches past the buffer, addresses bytes the client cannot
        // have meant; applying it would corrupt the document
        if (offsets.first > offsets.second || offsets.second >= textView.size()) {
            throw std::runtime_error(fmt::format("Range out of bounds: {}:{}-{}:{} / {} bytes",
                                                 range.start.line, range.start.character,
                                                 range.end.line, range.end.character,
                                                 textView.size()));
        }
        return offsets;
    };

    auto ensureNullTerminated = [&]() {
        if (buffer.empty() || buffer.back() != '\0')
            buffer.push_back('\0');
    };

    // Single change (most common)
    rfl::visit(
        [&](const auto& change) {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                auto offsets = getOffsets(change.range);
                buffer.append(textView.begin(), textView.begin() + offsets.first);
                buffer.append(change.text.begin(), change.text.end());
                buffer.append(textView.begin() + offsets.second, textView.end());
            }
            else {
                buffer.append(change.text.begin(), change.text.end());
            }
        },
        contentChanges[0]);
    ensureNullTerminated();

    // More than one change is rare- typically things like rename actions, or if there's some lag.
    for (size_t i = 1; i < contentChanges.size(); i++) {
        textView = std::string_view{buffer.data(), buffer.size()};
        lineOffsets.clear();
        rfl::visit(
            [&](const auto& change) {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                    auto offsets = getOffsets(change.range);
                    // handle deletes
                    if (offsets.second > offsets.first) {
                        buffer.erase(buffer.begin() + offsets.first,
                                     buffer.begin() + offsets.second);
                    }
                    // handle inserts
                    buffer.insert(buffer.begin() + offsets.first, change.text.begin(),
                                  change.text.end());
                }
                else {
                    // WholeDocument collapses all prior changes
                    buffer.clear();
                    buffer.append(change.text.begin(), change.text.end());
                    ensureNullTerminated();
                }
            },
            contentChanges[i]);
    }
    m_buffer = m_sourceManager.replaceBuffer(m_buffer.id, std::move(buffer));

    // Invalidate pointers to old buffer
    m_tree.reset();
    m_analysis.reset();
    m_includeCompilation.reset();
}
bool SlangDoc::reloadBuffer() {
    auto result = m_sourceManager.reloadBuffer(m_buffer.id);
    if (!result) {
        ERROR("Failed to re-read buffer for {}: {}", m_uri.getPath(), result.error().message());
        return false;
    }
    m_buffer = *result;
    m_tree.reset();
    m_analysis.reset();
    m_includeCompilation.reset();
    return true;
}

void SlangDoc::issueParseDiagnostics(DiagnosticEngine& diagEngine) {
    for (auto& diag : getSyntaxTree()->diagnostics()) {
        if (m_includeCompilation &&
            m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id)
            continue;
        diagEngine.issue(diag);
    }
}

void SlangDoc::issueDiagnosticsTo(DiagnosticEngine& diagEngine, const lsp::RequestContext& ctx) {
    // Issue compilation diagnostics
    auto analysis = refreshAnalysis(ctx);

    // Parse diags (just this tree, others will be handled by their SlangDoc objects
    for (auto& diag : getSyntaxTree()->diagnostics()) {
        if (m_includeCompilation &&
            m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id)
            continue;
        diagEngine.issue(diag);
    }

    // Parse and shallow compilation diagnostics
    // There will be many diags outside the buffer, like unknown modules.
    const auto& semanticDiagnostics = analysis->getSemanticDiagnostics();
    ctx.throwIfCancelled("before publishing semantic diagnostics");
    for (auto& diag : semanticDiagnostics) {
        if (m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id) {
            continue;
        }
        if (diag.code == slang::diag::MaxInstanceDepthExceeded) {
            continue;
        }
        diagEngine.issue(diag);
    }

    // Analysis on the shallow compilation (unused, multidriven, etc)
    auto analysisDiagnostics = analysis->getAnalysisDiags();
    ctx.throwIfCancelled("before publishing analysis diagnostics");
    for (auto& diag : analysisDiagnostics) {
        if (m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id) {
            continue;
        }
        diagEngine.issue(diag);
    }
}

std::vector<lsp::Range> SlangDoc::getInactiveRegions(const lsp::RequestContext& ctx) {
    ctx.throwIfCancelled("before collecting inactive regions");
    std::vector<lsp::Range> result;
    auto analysis = getAnalysis(ctx);
    result.reserve(analysis->syntaxes.disabledRegions.size());

    for (const auto& region : analysis->syntaxes.disabledRegions) {
        result.push_back(toRange(region, m_sourceManager));
    }

    return result;
}

} // namespace server
