// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "document/SemanticTokens.h"
#include "utils/GoldenTest.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slang/text/SourceManager.h"

using namespace server;
using namespace slang;

namespace {

/// A semantic token decoded from the wire format into absolute document coordinates.
struct DecodedToken {
    lsp::uint line = 0;
    lsp::uint character = 0;
    lsp::uint length = 0;
    SemanticTokenType type = SemanticTokenType::Count;
    uint32_t modifiers = 0;

    bool operator==(const DecodedToken&) const = default;
};

/// Undo the relative encoding the server produces; this is what a client does.
std::vector<DecodedToken> decodeTokens(const std::vector<lsp::uint>& data) {
    std::vector<DecodedToken> result;
    if (data.size() % 5 != 0)
        return result;

    lsp::uint line = 0;
    lsp::uint character = 0;
    for (size_t i = 0; i < data.size(); i += 5) {
        line += data[i];
        // deltaStart is relative to the previous token only while we stay on the line
        character = data[i] == 0 ? character + data[i + 1] : data[i + 1];
        result.push_back(DecodedToken{.line = line,
                                      .character = character,
                                      .length = data[i + 2],
                                      .type = static_cast<SemanticTokenType>(data[i + 3]),
                                      .modifiers = data[i + 4]});
    }
    return result;
}

std::string describeToken(SemanticTokenType type, uint32_t modifiers) {
    auto types = semanticTokenTypeNames();
    auto modifierNames = semanticTokenModifierNames();
    auto index = static_cast<size_t>(type);
    if (index >= types.size())
        return "?";

    std::string result(types[index]);
    bool any = false;
    for (size_t i = 0; i < modifierNames.size(); i++) {
        if (modifiers & (1u << i)) {
            result += any ? "," : "[";
            result += modifierNames[i];
            any = true;
        }
    }
    if (any)
        result += "]";
    return result;
}

bool hasModifier(uint32_t modifiers, SemanticTokenModifier modifier) {
    return (modifiers & modifierBit(modifier)) != 0;
}

/// A token located in the document text by byte offsets.
struct PositionedToken {
    lsp::uint start = 0;
    lsp::uint end = 0;
    SemanticTokenType type = SemanticTokenType::Count;
    uint32_t modifiers = 0;
};

bool hasModifier(const PositionedToken& token, SemanticTokenModifier modifier) {
    return hasModifier(token.modifiers, modifier);
}

/// Walks `character` UTF-16 code units into the line to find the byte offset.
lsp::uint byteOffsetOf(std::string_view text, const std::vector<size_t>& lineOffsets,
                       lsp::uint line, lsp::uint character) {
    if (line >= lineOffsets.size())
        return static_cast<lsp::uint>(text.size());

    size_t offset = lineOffsets[line];
    while (character > 0 && offset < text.size() && text[offset] != '\n' && text[offset] != '\r') {
        auto byte = static_cast<unsigned char>(text[offset]);
        size_t advance = byte < 0x80             ? 1
                         : (byte & 0xE0) == 0xC0 ? 2
                         : (byte & 0xF0) == 0xE0 ? 3
                                                 : 4;
        // A four byte sequence is a surrogate pair, which counts as two units
        uint32_t units = advance == 4 ? 2u : 1u;
        character = units >= character ? 0u : character - units;
        offset += advance;
    }
    return static_cast<lsp::uint>(offset);
}

std::vector<PositionedToken> locateTokens(std::string_view text,
                                          const std::vector<DecodedToken>& decoded) {
    std::vector<size_t> lineOffsets;
    SourceManager::computeLineOffsets(text, lineOffsets);

    std::vector<PositionedToken> result;
    result.reserve(decoded.size());
    for (const auto& token : decoded) {
        result.push_back(PositionedToken{
            .start = byteOffsetOf(text, lineOffsets, token.line, token.character),
            .end = byteOffsetOf(text, lineOffsets, token.line, token.character + token.length),
            .type = token.type,
            .modifiers = token.modifiers,
        });
    }
    return result;
}

/// All tokens of a document, located in its text.
std::vector<PositionedToken> positionedTokens(::DocumentHandle& hdl,
                                              std::optional<lsp::Range> range = {}) {
    auto data = range ? hdl.getSemanticTokens(*range) : hdl.getSemanticTokens();
    return locateTokens(hdl.getText(), decodeTokens(data));
}

std::optional<PositionedToken> tokenAt(const std::vector<PositionedToken>& tokens,
                                       lsp::uint offset) {
    auto it = std::partition_point(tokens.begin(), tokens.end(), [&](const PositionedToken& token) {
        return token.start <= offset;
    });
    if (it == tokens.begin())
        return std::nullopt;

    --it;
    return offset < it->end ? std::optional(*it) : std::nullopt;
}

/// The first token whose text is exactly `name`, which avoids matching a substring of
/// another identifier. Pass `occurrence` to skip that many earlier matches.
std::optional<PositionedToken> findNamedToken(::DocumentHandle& hdl, std::string_view name,
                                              size_t occurrence = 0) {
    auto text = hdl.getText();
    size_t seen = 0;
    for (const auto& token : positionedTokens(hdl)) {
        if (text.substr(token.start, token.end - token.start) != name)
            continue;
        if (seen == occurrence)
            return token;
        seen++;
    }
    return std::nullopt;
}

/// The token covering the byte offset of `anchor` plus `offsetInAnchor`, so a caller can
/// anchor on surrounding punctuation and point at the name inside it.
std::optional<PositionedToken> tokenAtAnchor(::DocumentHandle& hdl, std::string_view anchor,
                                             size_t offsetInAnchor = 0) {
    auto text = hdl.getText();
    auto pos = text.find(anchor);
    if (pos == std::string::npos)
        return std::nullopt;
    return tokenAt(positionedTokens(hdl), static_cast<lsp::uint>(pos + offsetInAnchor));
}

/// Renders the semantic token covering each position of a document, for golden tests.
struct SemanticElement {
    SemanticTokenType type = SemanticTokenType::Count;
    uint32_t modifiers = 0;

    bool operator==(const SemanticElement&) const = default;
};

class SemanticTokenScanner : public DocumentScanner<SemanticElement> {
protected:
    std::vector<PositionedToken> m_tokens;

    std::optional<SemanticElement> getElementAt(::DocumentHandle* hdl, lsp::uint offset) override {
        if (m_tokens.empty())
            m_tokens = positionedTokens(*hdl);

        auto token = tokenAt(m_tokens, offset);
        if (!token)
            return std::nullopt;
        return SemanticElement{.type = token->type, .modifiers = token->modifiers};
    }

    void processElementTransition(::DocumentHandle*, SourceManager&, lsp::uint) override {
        auto& element = std::get<SemanticElement>(*prevElement);
        test.record(fmt::format(" {}\n", describeToken(element.type, element.modifiers)));
    }
};

/// A document covering the declaration kinds we classify.
constexpr std::string_view KINDS_SOURCE = R"(
`define WIDTH 8
package pkg;
    typedef logic [7:0] byte_t;
    parameter int PKG_PARAM = 1;
endpackage

interface bus_if;
    logic ready;
    modport mp(input ready);
endinterface

module top #(parameter int W = 3, localparam int L = 4) (
    input logic clk,
    output wire [W-1:0] data
);
    typedef enum { IDLE, RUN } state_e;
    typedef struct packed { logic a; } str_t;

    wire net_sig;
    logic var_sig;
    byte_t byte_var;
    str_t struct_var;
    state_e enum_var;
    genvar gen_var;
    bus_if bus();

    child u_child(.clk(clk), .data(data));

    function automatic int f(int arg);
        return arg;
    endfunction

    initial begin : lbl
        $display("hi");
    end
endmodule

module child(input logic clk, output logic [7:0] data);
endmodule
)";

} // namespace

TEST_CASE("SemanticTokenEncoding") {
    std::string text = "module top;\n  logic clk;\nendmodule\n";
    std::vector<SemanticToken> tokens = {
        SemanticToken{.offset = 7,
                      .length = 3,
                      .type = SemanticTokenType::Class,
                      .modifiers = modifierBit(SemanticTokenModifier::Declaration)},
        SemanticToken{.offset = 20,
                      .length = 3,
                      .type = SemanticTokenType::Variable,
                      .modifiers = modifierBit(SemanticTokenModifier::Declaration)},
    };

    auto decoded = decodeTokens(encodeSemanticTokens(text, tokens));
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0] == DecodedToken{.line = 0,
                                     .character = 7,
                                     .length = 3,
                                     .type = SemanticTokenType::Class,
                                     .modifiers = modifierBit(SemanticTokenModifier::Declaration)});
    CHECK(decoded[1] == DecodedToken{.line = 1,
                                     .character = 8,
                                     .length = 3,
                                     .type = SemanticTokenType::Variable,
                                     .modifiers = modifierBit(SemanticTokenModifier::Declaration)});

    // The wire format is groups of five integers
    CHECK(encodeSemanticTokens(text, tokens).size() == 10);
    CHECK(encodeSemanticTokens(text, {}).empty());
}

TEST_CASE("SemanticTokenEncodingUTF16") {
    // A surrogate pair occupies two UTF-16 code units but four bytes, so the character
    // offsets the client sees are not byte offsets.
    std::string text = "initial $display(\"\xF0\x9D\x84\x9E\"); assign bb = cc;\n";
    CHECK(text.find("bb") == 33);

    std::vector<SemanticToken> tokens = {
        SemanticToken{.offset = 33, .length = 2, .type = SemanticTokenType::Variable},
    };

    auto decoded = decodeTokens(encodeSemanticTokens(text, tokens));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].character == 31);
    CHECK(decoded[0].length == 2);
    CHECK(decoded[0].line == 0);

    // Multi byte characters outside of the BMP are one unit, and their byte length differs
    std::string utf8 = "logic \xE6\x95\xB0\xE6\x8D\xAE;\nassign \xE6\x95\xB0\xE6\x8D\xAE = 1;\n";
    std::vector<SemanticToken> shortTokens = {
        SemanticToken{.offset = 6, .length = 6, .type = SemanticTokenType::Variable},
        SemanticToken{.offset = 21, .length = 6, .type = SemanticTokenType::Variable},
    };

    decoded = decodeTokens(encodeSemanticTokens(utf8, shortTokens));
    REQUIRE(decoded.size() == 2);
    CHECK(
        decoded[0] ==
        DecodedToken{.line = 0, .character = 6, .length = 2, .type = SemanticTokenType::Variable});
    CHECK(
        decoded[1] ==
        DecodedToken{.line = 1, .character = 7, .length = 2, .type = SemanticTokenType::Variable});
}

TEST_CASE("SemanticTokenEncodingSkipsInvalidTokens") {
    std::string text = "a\nbb\nccc\n";
    std::vector<SemanticToken> tokens = {
        // Fine
        SemanticToken{.offset = 0, .length = 1, .type = SemanticTokenType::Variable},
        // Crosses a line boundary, which the protocol cannot describe
        SemanticToken{.offset = 2, .length = 3, .type = SemanticTokenType::Variable},
        // Past the end of the document
        SemanticToken{.offset = 100, .length = 3, .type = SemanticTokenType::Variable},
        // Empty
        SemanticToken{.offset = 0, .length = 0, .type = SemanticTokenType::Variable},
    };

    auto decoded = decodeTokens(encodeSemanticTokens(text, tokens));
    REQUIRE(decoded.size() == 1);
    CHECK(
        decoded[0] ==
        DecodedToken{.line = 0, .character = 0, .length = 1, .type = SemanticTokenType::Variable});
}

TEST_CASE("SemanticTokenEncodingRange") {
    std::string text = "aa\nbb\ncc\n";
    std::vector<SemanticToken> tokens = {
        SemanticToken{.offset = 0, .length = 2, .type = SemanticTokenType::Variable},
        SemanticToken{.offset = 3, .length = 2, .type = SemanticTokenType::Net},
        SemanticToken{.offset = 6, .length = 2, .type = SemanticTokenType::Instance},
    };

    lsp::Range range{.start = {.line = 1, .character = 0}, .end = {.line = 3, .character = 0}};
    auto decoded = decodeTokens(encodeSemanticTokens(text, tokens, &range));
    REQUIRE(decoded.size() == 2);
    // The first token is always absolute, even when the range starts later
    CHECK(decoded[0] ==
          DecodedToken{.line = 1, .character = 0, .length = 2, .type = SemanticTokenType::Net});
    CHECK(
        decoded[1] ==
        DecodedToken{.line = 2, .character = 0, .length = 2, .type = SemanticTokenType::Instance});

    // An empty range yields nothing
    lsp::Range empty{.start = {.line = 2, .character = 0}, .end = {.line = 2, .character = 0}};
    CHECK(encodeSemanticTokens(text, tokens, &empty).empty());
}

TEST_CASE("SemanticTokenLegend") {
    JsonGoldenTest golden;
    golden.record("tokenTypes", std::vector<std::string>(semanticTokenTypeNames().begin(),
                                                         semanticTokenTypeNames().end()));
    golden.record("tokenModifiers", std::vector<std::string>(semanticTokenModifierNames().begin(),
                                                             semanticTokenModifierNames().end()));
}

TEST_CASE("SemanticTokensAreAdvertised") {
    ServerHarness server;
    auto result = server.getInitialize(lsp::InitializeParams{});

    REQUIRE(result.capabilities.semanticTokensProvider.has_value());
    auto& options = rfl::get<lsp::SemanticTokensOptions>(
        *result.capabilities.semanticTokensProvider);
    CHECK(options.full.has_value());
    CHECK(options.range.value_or(false));
    CHECK(options.legend.tokenTypes.size() == semanticTokenTypeNames().size());
    CHECK(options.legend.tokenModifiers.size() == semanticTokenModifierNames().size());
    CHECK(options.legend.tokenTypes.front() == "namespace");
}

TEST_CASE("SemanticTokensDeclarationKinds") {
    ServerHarness server("");
    auto doc = server.openFile("semantic_kinds.sv", std::string(KINDS_SOURCE));

    auto check = [&](std::string_view name, SemanticTokenType expected) {
        auto token = findNamedToken(doc, name);
        if (!token)
            FAIL("no semantic token for " << std::string(name));
        else
            CHECK(token->type == expected);
    };

    // Declarations
    check("top", SemanticTokenType::Class);
    check("WIDTH", SemanticTokenType::Macro);
    check("pkg", SemanticTokenType::Namespace);
    check("byte_t", SemanticTokenType::Type);
    check("PKG_PARAM", SemanticTokenType::Parameter);
    check("bus_if", SemanticTokenType::Interface);
    check("mp", SemanticTokenType::Modport);
    check("clk", SemanticTokenType::Port);
    check("data", SemanticTokenType::Port);
    check("W", SemanticTokenType::Parameter);
    check("IDLE", SemanticTokenType::EnumMember);
    check("state_e", SemanticTokenType::Type);
    check("str_t", SemanticTokenType::Type);
    check("net_sig", SemanticTokenType::Net);
    check("var_sig", SemanticTokenType::Variable);
    check("byte_var", SemanticTokenType::Variable);
    check("struct_var", SemanticTokenType::Variable);
    check("enum_var", SemanticTokenType::Variable);
    check("bus", SemanticTokenType::Instance);
    check("u_child", SemanticTokenType::Instance);
    check("f", SemanticTokenType::Function);
    check("arg", SemanticTokenType::Parameter);
    check("lbl", SemanticTokenType::Label);
    check("child", SemanticTokenType::Class);
    check("$display", SemanticTokenType::Function);

    auto moduleToken = findNamedToken(doc, "top");
    REQUIRE(moduleToken.has_value());
    CHECK(hasModifier(*moduleToken, SemanticTokenModifier::Definition));
    CHECK(hasModifier(*moduleToken, SemanticTokenModifier::Declaration));

    // A localparam and a genvar are read only
    auto localparam = findNamedToken(doc, "L");
    REQUIRE(localparam.has_value());
    CHECK(localparam->type == SemanticTokenType::Parameter);
    CHECK(hasModifier(*localparam, SemanticTokenModifier::ReadOnly));

    auto genvar = findNamedToken(doc, "gen_var");
    REQUIRE(genvar.has_value());
    CHECK(genvar->type == SemanticTokenType::Variable);
    CHECK(hasModifier(*genvar, SemanticTokenModifier::ReadOnly));

    auto systemTask = findNamedToken(doc, "$display");
    REQUIRE(systemTask.has_value());
    CHECK(hasModifier(*systemTask, SemanticTokenModifier::DefaultLibrary));
}

TEST_CASE("SemanticTokensReferences") {
    ServerHarness server("");
    auto doc = server.openFile("semantic_refs.sv", R"(
module top;
    logic sig, other;
    assign sig = other;
    always_comb begin
        other = sig;
    end
endmodule
)");

    auto declaration = findNamedToken(doc, "sig");
    REQUIRE(declaration.has_value());
    CHECK(declaration->type == SemanticTokenType::Variable);
    CHECK(hasModifier(*declaration, SemanticTokenModifier::Declaration));

    // References are colored like their declaration, but aren't marked as declarations
    auto reference = findNamedToken(doc, "sig", 1);
    REQUIRE(reference.has_value());
    CHECK(reference->type == SemanticTokenType::Variable);
    CHECK(!hasModifier(*reference, SemanticTokenModifier::Declaration));

    // The same holds inside a procedural block and for a use before the declaration site
    auto assignment = findNamedToken(doc, "other", 1);
    REQUIRE(assignment.has_value());
    CHECK(assignment->type == SemanticTokenType::Variable);
    CHECK(!hasModifier(*assignment, SemanticTokenModifier::Declaration));

    auto inBlock = findNamedToken(doc, "sig", 2);
    REQUIRE(inBlock.has_value());
    CHECK(inBlock->type == SemanticTokenType::Variable);
    CHECK(!hasModifier(*inBlock, SemanticTokenModifier::Declaration));
}

TEST_CASE("SemanticTokensInactiveRegions") {
    ServerHarness server("");
    auto doc = server.openFile("semantic_inactive.sv", R"(
`define ON
module top;
`ifdef UNDEFINED
    logic disabled_sig;
`endif
    logic enabled_sig;
endmodule
)");

    CHECK(findNamedToken(doc, "enabled_sig").has_value());
    // Disabled regions are rendered dimmed, so they don't get highlighted
    CHECK(!findNamedToken(doc, "disabled_sig").has_value());
}

TEST_CASE("SemanticTokensConfig") {
    ServerHarness server("");
    auto doc = server.openFile("semantic_config.sv", R"(
module top;
    logic sig;
endmodule
)");

    CHECK(!doc.getSemanticTokens().empty());

    // The feature can be turned off entirely from the server config
    auto config = server.getConfig();
    config.semanticTokens.value().enabled = false;
    server.loadConfig(config);
    CHECK(doc.getSemanticTokens().empty());

    config.semanticTokens.value().enabled = true;
    server.loadConfig(config);
    CHECK(!doc.getSemanticTokens().empty());
}

TEST_CASE("SemanticTokensUpdateOnEdit") {
    ServerHarness server("");
    auto doc = server.openFile("semantic_edit.sv", R"(
module top;
    wire sig;
endmodule
)");

    auto token = findNamedToken(doc, "sig");
    REQUIRE(token.has_value());
    CHECK(token->type == SemanticTokenType::Net);

    // The cached tokens belong to the analysis, so an edit must produce new ones
    auto pos = doc.getText().find("wire");
    doc.erase(pos, pos + 4);
    doc.insert(pos, "logic");
    doc.publishChanges();

    token = findNamedToken(doc, "sig");
    REQUIRE(token.has_value());
    CHECK(token->type == SemanticTokenType::Variable);
}

TEST_CASE("SemanticTokensPortConnections") {
    ServerHarness server("");
    // A `.name` connection names the formal port, both in the shorthand and explicit
    // forms, so the two should look the same even though the connected signal is a net
    // for inputs and a variable for outputs
    auto doc = server.openFile("semantic_ports.sv", R"(
module axi_lite_2_reg (
    input logic clk,
    input logic arstn,
    input logic s_awvalid,
    output logic s_awready,
    input logic reg_data
);
endmodule

module top (
    input logic clk,
    input logic arstn,
    input logic s_awvalid,
    output logic s_awready
);
    logic reg_data;

    axi_lite_2_reg u_axil2reg(
        .clk,
        .arstn,
        .s_awvalid,
        .s_awready,
        .reg_data,
        .*
    );

    axi_lite_2_reg u_explicit(
        .clk(clk),
        .arstn(arstn),
        .s_awvalid(s_awvalid),
        .s_awready(s_awready),
        .reg_data(reg_data)
    );
endmodule
)");

    // Shorthand connections name the formal port
    for (auto anchor : {".clk,", ".arstn,", ".s_awvalid,", ".s_awready,", ".reg_data,"}) {
        auto token = tokenAtAnchor(doc, anchor, 1);
        if (!token)
            FAIL("no semantic token for " << anchor);
        else {
            CHECK(token->type == SemanticTokenType::Port);
            // A connection is not a declaration; clients color the two differently
            CHECK(!hasModifier(*token, SemanticTokenModifier::Declaration));
        }
    }

    // The explicit form agrees, and the signal inside the parens keeps its own type
    auto formal = tokenAtAnchor(doc, ".clk(", 1);
    REQUIRE(formal.has_value());
    CHECK(formal->type == SemanticTokenType::Port);
    CHECK(!hasModifier(*formal, SemanticTokenModifier::Declaration));

    auto actual = tokenAtAnchor(doc, "(clk)", 1);
    REQUIRE(actual.has_value());
    CHECK(actual->type == SemanticTokenType::Net);

    // The port declarations themselves are ports too, and are marked as declarations
    auto declaration = findNamedToken(doc, "clk");
    REQUIRE(declaration.has_value());
    CHECK(declaration->type == SemanticTokenType::Port);
    CHECK(hasModifier(*declaration, SemanticTokenModifier::Declaration));

    SemanticTokenScanner scanner;
    scanner.scanDocument(doc);
}

TEST_CASE("SemanticTokensScopedNameReferences") {
    ServerHarness server("");
    // The member of a `::` lookup is not visible in the enclosing scope, so it is resolved
    // through the qualified name, the same way hover and goto resolve it
    auto doc = server.openFile("semantic_scoped.sv", R"(
package pkg;
    localparam int WIDTH = 8;
    parameter int PBAR = 5;
endpackage

module child #(parameter int W = 1) (input logic [W-1:0] d);
endmodule

module top (
    input logic [pkg::WIDTH-1:0] data
);
    typedef enum logic [1:0] {LOC_A, LOC_B} loc_e;
    logic [pkg::WIDTH-1:0] sig;
    localparam int X = pkg::PBAR;
    localparam int Y = loc_e::LOC_A;
    localparam int Z = pkg::MISSING;
    child #(.W(pkg::WIDTH)) u_child (.d(sig));
endmodule
)");

    // The qualifier is a namespace, and the member is colored like its declaration
    auto qualifier = findNamedToken(doc, "pkg", 1);
    REQUIRE(qualifier.has_value());
    CHECK(qualifier->type == SemanticTokenType::Namespace);

    auto declaration = findNamedToken(doc, "WIDTH");
    REQUIRE(declaration.has_value());
    CHECK(declaration->type == SemanticTokenType::Parameter);
    CHECK(hasModifier(*declaration, SemanticTokenModifier::Declaration));
    CHECK(hasModifier(*declaration, SemanticTokenModifier::ReadOnly));

    // Every qualified reference, whether it is a dimension, an instance parameter or an
    // expression
    for (size_t occurrence : {1, 2, 3}) {
        auto reference = findNamedToken(doc, "WIDTH", occurrence);
        if (!reference)
            FAIL("no semantic token for WIDTH occurrence " << occurrence);
        else {
            CHECK(reference->type == SemanticTokenType::Parameter);
            CHECK(!hasModifier(*reference, SemanticTokenModifier::Declaration));
            CHECK(hasModifier(*reference, SemanticTokenModifier::ReadOnly));
        }
    }

    // A reference carries the modifiers of its declaration, apart from the declaration
    // marker itself
    auto parameterDeclaration = findNamedToken(doc, "PBAR");
    REQUIRE(parameterDeclaration.has_value());
    auto parameter = findNamedToken(doc, "PBAR", 1);
    REQUIRE(parameter.has_value());
    CHECK(parameter->type == parameterDeclaration->type);
    CHECK(parameter->modifiers ==
          (parameterDeclaration->modifiers & ~modifierBit(SemanticTokenModifier::Declaration)));

    // Enum members resolve through their type
    auto enumMember = findNamedToken(doc, "LOC_A", 1);
    REQUIRE(enumMember.has_value());
    CHECK(enumMember->type == SemanticTokenType::EnumMember);
    CHECK(!hasModifier(*enumMember, SemanticTokenModifier::Declaration));

    // A name that does not resolve has no token at all, and neither does the separator
    CHECK(!findNamedToken(doc, "MISSING").has_value());
    CHECK(!findNamedToken(doc, "::").has_value());

    SemanticTokenScanner scanner;
    scanner.scanDocument(doc);
}

TEST_CASE("SemanticTokensAll") {
    /// Semantic tokens on the comprehensive all.sv test file
    ServerHarness server("");
    auto hdl = server.openFile("all.sv");

    SemanticTokenScanner scanner;
    scanner.scanDocument(hdl);
}
