// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

// Contract tests between the semantic token legend advertised by the server and the
// client side highlighting configuration. A token type that is not part of the standard
// LSP set is only colored when the client knows about it, so the legend, the editor
// mappings, and the documentation have to move together.

#include "document/SemanticTokens.h"
#include "utils/ServerHarness.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace server;

namespace {

constexpr std::string_view VSCODE_PACKAGE = "clients/vscode/package.json";
constexpr std::string_view DOCS_PAGE = "docs/features/semantic-tokens.md";
constexpr std::string_view DOCS_NAV_ENTRY = "features/semantic-tokens.md";

/// Read a file of this repository. Tests run from a build directory, which is not
/// necessarily a subdirectory of the checkout, so the file is looked up relative to this
/// source file first and then in every parent of the working directory. A missing file is
/// a warning rather than a failure, so that a build tree without the sources still passes.
std::optional<std::string> readRepoFile(std::string_view relative) {
    std::vector<std::filesystem::path> roots;
    auto sourceDir = std::filesystem::path(__FILE__).parent_path();
    if (sourceDir.is_absolute()) {
        // __FILE__ is <checkout>/tests/cpp/<this file>
        roots.push_back(sourceDir.parent_path().parent_path());
    }

    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    for (int i = 0; i < 8 && !dir.empty(); i++) {
        roots.push_back(dir);
        dir = dir.parent_path();
    }

    for (const auto& root : roots) {
        auto path = root / relative;
        if (!std::filesystem::exists(path, ec))
            continue;

        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        std::ostringstream contents;
        contents << stream.rdbuf();
        return contents.str();
    }

    WARN("Could not find " << relative << " from " << std::filesystem::current_path()
                           << "; skipping the client contract check");
    return std::nullopt;
}

/// The legend of the running server, as advertised to clients.
std::vector<std::string> getLegendTokenTypes() {
    ServerHarness server;
    auto result = server.getInitialize(lsp::InitializeParams{});

    REQUIRE(result.capabilities.semanticTokensProvider.has_value());
    auto& options = rfl::get<lsp::SemanticTokensOptions>(
        *result.capabilities.semanticTokensProvider);
    return options.legend.tokenTypes;
}

/// The token types that are not part of the standard LSP set, and therefore need a client
/// side mapping to be colored.
std::vector<std::string> customTokenTypes() {
    auto names = semanticTokenTypeNames();
    std::vector<std::string> result;
    for (size_t i = 0; i < names.size(); i++) {
        if (isCustomSemanticTokenType(static_cast<SemanticTokenType>(i)))
            result.emplace_back(names[i]);
    }
    return result;
}

/// The subset of the vscode client's package.json that this contract is about.
struct VscodeScopeEntry {
    std::optional<std::string> language;
    std::map<std::string, std::vector<std::string>> scopes;
};

struct VscodeLanguage {
    std::string id;
};

struct VscodeContributes {
    std::vector<VscodeScopeEntry> semanticTokenScopes;
    std::vector<VscodeLanguage> languages;
};

struct VscodePackage {
    VscodeContributes contributes;
};

/// The token type of a `semanticTokenScopes` key, which may carry modifiers after a dot,
/// e.g. "port.declaration".
std::string scopeEntryType(const std::string& key) {
    auto dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(0, dot);
}

} // namespace

TEST_CASE("SemanticTokenLegendListsCustomTypesLast") {
    auto legend = getLegendTokenTypes();

    auto names = semanticTokenTypeNames();
    REQUIRE(legend.size() == names.size());
    for (size_t i = 0; i < names.size(); i++)
        CHECK(legend[i] == names[i]);

    auto isCustom = [](size_t index) {
        return isCustomSemanticTokenType(static_cast<SemanticTokenType>(index));
    };

    // Clients color the standard types out of the box, and use the documented position of
    // the custom ones (see the comment on SemanticTokenType) to tell the two sets apart.
    size_t firstCustom = legend.size();
    for (size_t i = 0; i < legend.size(); i++) {
        if (isCustom(i)) {
            firstCustom = i;
            break;
        }
    }
    REQUIRE(firstCustom < legend.size());
    for (size_t i = 0; i < legend.size(); i++)
        CHECK(isCustom(i) == (i >= firstCustom));
}

TEST_CASE("VscodeSemanticTokenScopesCoverCustomTypes") {
    auto text = readRepoFile(VSCODE_PACKAGE);
    if (!text)
        return;

    auto package = rfl::json::read<VscodePackage>(*text);
    REQUIRE(package.has_value());
    auto& contributes = package->contributes;

    // Every legend type referenced by a scope mapping has to exist, so that a renamed or
    // removed token type does not silently fall back to the TextMate colors.
    auto legend = getLegendTokenTypes();
    std::set<std::string> legendTypes(legend.begin(), legend.end());
    for (const auto& entry : contributes.semanticTokenScopes) {
        for (const auto& [key, scopes] : entry.scopes) {
            INFO("scope mapping '" << key << "' of language " << entry.language.value_or("<any>"));
            CHECK(legendTypes.count(scopeEntryType(key)) == 1);
            CHECK(!scopes.empty());
        }
    }

    // The server classifies Verilog files too, so every language the extension registers
    // HDL files for needs a mapping of each custom type. A mapping without a language
    // applies to all languages.
    std::set<std::string> globalScopes;
    std::map<std::string, std::set<std::string>> languageScopes;
    for (const auto& entry : contributes.semanticTokenScopes) {
        auto& target = entry.language ? languageScopes[*entry.language] : globalScopes;
        for (const auto& key : entry.scopes | std::views::keys)
            target.insert(scopeEntryType(key));
    }

    std::vector<std::string> hdlLanguages;
    for (const auto& language : contributes.languages) {
        if (language.id.find("verilog") != std::string::npos)
            hdlLanguages.push_back(language.id);
    }
    REQUIRE(!hdlLanguages.empty());

    for (const auto& language : hdlLanguages) {
        INFO("language '" << language << "'");
        for (const auto& type : customTokenTypes()) {
            INFO("custom token type '" << type << "'");
            CHECK((globalScopes.count(type) == 1 || languageScopes[language].count(type) == 1));
        }
    }
}

TEST_CASE("SemanticTokenDocsCoverCustomTypes") {
    auto text = readRepoFile(DOCS_PAGE);
    if (!text)
        return;

    for (const auto& type : customTokenTypes()) {
        INFO("custom token type '" << type << "'");
        CHECK(text->find("`" + type + "`") != std::string::npos);
    }

    // A page that is not in the navigation is not published
    auto nav = readRepoFile("mkdocs.yaml");
    if (nav)
        CHECK(nav->find(DOCS_NAV_ENTRY) != std::string::npos);
}
