// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//
// Accepting a completion must leave the document parseable. This walks every completion path,
// applies each item the way a client would, and re-parses the result, so that a suggestion can
// never join onto the surrounding text and produce something else (the classic `logic` + a name
// with no separator) or replace more than the word being typed.

#include "util/Logging.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"

using namespace slang;

namespace {

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '`';
}

/// The text a client ends up with when a snippet is accepted with no further typing: placeholders
/// keep the text they show, and empty tab stops contribute nothing.
std::string materializeSnippet(std::string_view snippet) {
    std::string result;
    for (size_t i = 0; i < snippet.size(); i++) {
        char c = snippet[i];
        if (c == '\\' && i + 1 < snippet.size() &&
            (snippet[i + 1] == '$' || snippet[i + 1] == '}' || snippet[i + 1] == '\\')) {
            result += snippet[++i];
            continue;
        }
        if (c != '$' || i + 1 == snippet.size()) {
            result += c;
            continue;
        }

        size_t j = i + 1;
        if (snippet[j] == '{')
            j++;
        size_t digitsStart = j;
        while (j < snippet.size() && std::isdigit(static_cast<unsigned char>(snippet[j])))
            j++;
        if (j == digitsStart) {
            result += c;
            continue;
        }
        if (j < snippet.size() && snippet[j] == ':') {
            // ${n:default} keeps the default text, which may itself contain placeholders
            size_t depth = 1;
            size_t end = j + 1;
            while (end < snippet.size() && depth > 0) {
                if (snippet[end] == '{')
                    depth++;
                else if (snippet[end] == '}')
                    depth--;
                if (depth == 0)
                    break;
                end++;
            }
            result += materializeSnippet(snippet.substr(j + 1, end - (j + 1)));
            i = end;
            continue;
        }
        // ${n} and $n are empty until the user fills them in
        i = j - 1;
        if (i + 1 < snippet.size() && snippet[i + 1] == '}')
            i++;
    }
    return result;
}

/// Error codes that a parse produced, so an incomplete document can be compared before and after.
/// Each parse gets its own source manager: they all use the document's path so that includes
/// resolve next to it, and a manager refuses to take the same path twice.
std::vector<std::string> parseErrors(std::string_view text, std::vector<size_t>* offsets = nullptr,
                                     std::string_view path = {}) {
    SourceManager sm;
    auto tree = syntax::SyntaxTree::fromText(text, sm, "source", path);
    std::vector<std::string> errors;
    for (auto& diag : tree->diagnostics()) {
        if (!diag.isError())
            continue;
        errors.push_back(std::string(toString(diag.code)));
        if (offsets)
            offsets->push_back(diag.location.offset());
    }
    std::sort(errors.begin(), errors.end());
    return errors;
}

std::string excerpt(std::string_view text, size_t offset) {
    auto start = offset > 25 ? offset - 25 : 0;
    auto end = std::min(text.size(), offset + 25);
    std::string result = std::string(text.substr(start, end - start));
    for (auto& c : result) {
        if (c == '\n')
            c = '|';
    }
    return result;
}

/// Cursor helpers: `at` is the first match in the document, `atFrom` the first match after an
/// anchor that makes the position unambiguous
auto at(std::string_view text) {
    return [text](DocumentHandle& doc) { return doc.after(std::string(text)); };
}

auto atFrom(std::string_view anchor, std::string_view text) {
    return [anchor, text](DocumentHandle& doc) {
        return doc.after(std::string(anchor)).after(std::string(text));
    };
}

} // namespace

TEST_CASE("Accepting any completion leaves the document parseable") {
    ServerHarness server("repo1");
    size_t checked = 0;
    size_t probes = 0;
    std::map<std::string, std::pair<size_t, std::string>> findings;
    std::map<std::string, std::string> duplicates;
    std::vector<std::string> summaries;

    // `allowed` lists the errors that an unfinished statement is expected to have: the item is the
    // first piece of what the user is writing, and continuing to type finishes it. Anything else
    // (a join onto a neighbour, a construct that cannot be completed where it was inserted, or an
    // error somewhere else in the document) is a failure.
    auto probe = [&](std::string_view path, std::string_view text,
                     const std::function<Cursor(DocumentHandle&)>& locate, std::string_view label,
                     std::optional<std::string> trigger = std::nullopt,
                     std::vector<std::string> allowed = {}) {
        auto doc = server.openFile(fmt::format("audit_{}.sv", probes++), std::string(text));
        auto cursor = locate(doc);
        auto fullText = doc.getText();
        // Parsing keeps the path so that includes resolve next to the document, but each parse
        // needs its own name
        auto docDir = std::filesystem::path(doc.m_uri.getPath()).parent_path();
        size_t parses = 0;
        auto parsePath = [&] {
            return (docDir / fmt::format("audit_parse_{}.sv", parses++)).string();
        };
        auto baseline = parseErrors(fullText, nullptr, parsePath());
        auto handles = cursor.getCompletions(trigger);

        auto loc = doc.getLocation(cursor.m_offset);
        REQUIRE(loc);
        auto context = server::CompletionContext::fromLocation(
            *doc.doc, *loc,
            lsp::CompletionContext{.triggerKind = trigger ? lsp::CompletionTriggerKind::TriggerCharacter
                                                          : lsp::CompletionTriggerKind::Invoked,
                                   .triggerCharacter = trigger});
        auto where = fmt::format("{} @ '{}'{} [{} kind, {} query]", path, label,
                                 trigger ? " trigger " + *trigger : "", toString(context.kind),
                                 toString(context.query->kind()));

        std::map<std::string, size_t> seen;
        std::map<int, size_t> kindCounts;
        for (auto& handle : handles) {
            handle.resolve();
            auto& item = handle.m_item;
            if (item.kind)
                kindCounts[static_cast<int>(*item.kind)]++;
            if (++seen[item.label] == 2) {
                duplicates[where] += fmt::format("{}, ", item.label);
            }

            REQUIRE(item.textEdit.has_value());
            auto edit = rfl::visit(
                [](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, lsp::TextEdit>)
                        return value;
                    else
                        return lsp::TextEdit{.range = value.replace, .newText = value.newText};
                },
                *item.textEdit);

            auto newText = item.insertTextFormat == lsp::InsertTextFormat::Snippet
                               ? materializeSnippet(edit.newText)
                               : edit.newText;
            auto start = static_cast<size_t>(doc.getOffset(edit.range.start));
            auto end = static_cast<size_t>(doc.getOffset(edit.range.end));
            auto replaced = std::string(fullText.substr(start, end - start));
            REQUIRE(end >= start);

            std::string problem;
            auto addProblem = [&](std::string_view what) {
                if (!problem.empty())
                    problem += "; ";
                problem += what;
            };

            // Joining onto a neighbour would silently change what the text means
            if (!newText.empty() && start > 0 && isWordChar(fullText[start - 1]) &&
                isWordChar(newText.front())) {
                addProblem("joins the text before it");
            }
            if (!newText.empty() && end < fullText.size() && isWordChar(fullText[end]) &&
                isWordChar(newText.back())) {
                addProblem("joins the text after it");
            }
            // Anything but the word being typed would be deleted
            if (std::ranges::any_of(replaced, [](char c) { return !isWordChar(c) && c != '.'; })) {
                addProblem(fmt::format("replaces '{}'", replaced));
            }

            auto result = fullText.substr(0, start) + newText + fullText.substr(end);
            std::vector<size_t> offsets;
            auto errors = parseErrors(result, &offsets, parsePath());

            // An error that was not there before means the edit broke the document
            auto remaining = baseline;
            for (size_t i = 0; i < errors.size(); i++) {
                auto it = std::ranges::find(remaining, errors[i]);
                if (it != remaining.end()) {
                    remaining.erase(it);
                    continue;
                }
                if (std::ranges::find(allowed, errors[i]) != allowed.end())
                    continue;
                addProblem(fmt::format("{} at '{}'", errors[i], excerpt(result, offsets[i])));
            }

            checked++;
            if (!problem.empty()) {
                // Group by position and kind, which is what a fix would key on
                auto kind = problem.substr(0, problem.find(" at '"));
                auto key = fmt::format("{}: '{}' {} (kind {})", where, item.label, kind,
                                       item.kind ? static_cast<int>(*item.kind) : -1);
                auto& entry = findings[key];
                entry.first++;
                if (entry.second.empty()) {
                    entry.second = fmt::format("newText='{}' problem='{}'", newText.substr(0, 40),
                                               problem);
                }
            }
        }

        std::string kinds;
        for (auto& [kind, count] : kindCounts)
            kinds += fmt::format("{}({}), ", kind, count);
        summaries.push_back(fmt::format("{}: {} items [{}]", where, handles.size(), kinds));
    };

    // ---------------------------------------------------------------- lexical
    probe("lexical/declaration-name",
          R"(
    module lex1;
        logic old_signal;
        logic [3:0] bus;
    endmodule
    )",
          at("logic old_si"), "logic old_si");

    probe("lexical/declaration-type-empty",
          R"(
    module lex2;
        int old_int;
        logic clk;
    endmodule
    )",
          at("int old_int"), "int old_int");

    probe("lexical/declaration-after-type",
          R"(
    module lex3;
        logic [3:0] bus;
        assign bus = 4'h0;
    endmodule
    )",
          at("logic [3:0] bus"), "logic [3:0] bus");

    probe("lexical/type-keyword-end",
          R"(
    module lex3b;
        logic foo;
    endmodule
    )",
          at("logic"), "logic|");

    probe("lexical/module-item-empty",
          R"(
    module lex4;
        logic clk;
    endmodule
    )",
          at("logic clk;"), "logic clk;",
          std::nullopt,
          // A keyword at the start of a module item is the first token of what the user is writing
          {"ExpectedDeclarator", "ExpectedContinuousAssignment", "ExpectedIdentifier",
           "ExpectedExpression", "ExpectedMember"});

    probe("lexical/statement",
          R"(
    module lex5;
        logic [7:0] x;
        logic [7:0] y;
        initial begin
            x = y;
        end
    endmodule
    )",
          atFrom("initial begin", "x = "), "x = ");

    probe("lexical/expression-mid-word",
          R"(
    module lex6;
        logic [7:0] x;
        logic [7:0] value;
        initial begin
            x = value;
        end
    endmodule
    )",
          atFrom("x = ", "val"), "x = val");

    probe("lexical/procedural-declaration",
          R"(
    module lex7;
        initial begin
            int local_var = 1;
        end
    endmodule
    )",
          at("int local_v"), "int local_v");

    probe("lexical/instance-type",
          R"(
    module inner_mod(input logic clk);
    endmodule
    module lex8;
        logic clk;
        inner_mod u_inst(.clk(clk));
    endmodule
    )",
          at("inner_mod u_in"), "inner_mod u_in");

    probe("lexical/instance-name-empty",
          R"(
    module inner_mod2(input logic clk);
    endmodule
    module lex9;
        logic clk;
        inner_mod2 u_inst2(.clk(clk));
    endmodule
    )",
          at("inner_mod2 u_inst2"), "inner_mod2 u_inst2");

    probe("lexical/generate-body",
          R"(
    module lex10;
        genvar gi;
        generate
            for (gi = 0; gi < 2; gi++) begin : gen_loop
                logic gen_sig;
            end
        endgenerate
    endmodule
    )",
          at("logic gen_si"), "logic gen_si");

    probe("lexical/parameter-name",
          R"(
    module lex11 #(parameter int old_param = 1);
    endmodule
    )",
          at("int old_par"), "int old_par");

    probe("lexical/port-type",
          R"(
    interface audit_if(input logic clk);
        logic valid;
    endinterface
    module lex12(output logic old_port, input logic clk);
    endmodule
    )",
          at("output logic old_po"), "output logic old_po");

    probe("lexical/port-name-empty",
          R"(
    module lex13(output logic old_port, input logic clk);
    endmodule
    )",
          atFrom("output logic ", "old_port"), "output logic old_port");

    // ------------------------------------------------------------- port lists
    constexpr std::string_view portMods = R"(
    module ports_inner(input logic clk, input logic [7:0] data, output logic q);
    endmodule
    )";

    probe("ports/connection-list-empty",
          std::string(portMods) + R"(
    module ports_top(
    );
        logic clk;
        logic [7:0] data;
        ports_inner u_ports(
        );
    endmodule
    )",
          at("u_ports("), "u_ports(");

    probe("ports/connection-dot",
          std::string(portMods) + R"(
    module ports_top2;
        logic clk;
        ports_inner u_ports2(
            .
        );
    endmodule
    )",
          atFrom("u_ports2(", "."), "u_ports2( .", ".");

    probe("ports/connection-name-mid-word",
          std::string(portMods) + R"(
    module ports_top3;
        logic clk;
        ports_inner u_ports3(
            .cl
        );
    endmodule
    )",
          atFrom("u_ports3(", ".cl"), "u_ports3( .cl");

    probe("ports/connection-after-comma",
          std::string(portMods) + R"(
    module ports_top4;
        logic clk;
        ports_inner u_ports4(
            .clk(clk),
        );
    endmodule
    )",
          atFrom("u_ports4(", ".clk(clk),"), "u_ports4( .clk(clk),");

    probe("ports/connection-after-connection",
          std::string(portMods) + R"(
    module ports_top5;
        logic clk;
        ports_inner u_ports5(
            .clk(clk)
        );
    endmodule
    )",
          atFrom("u_ports5(", ".clk(clk)"), "u_ports5( .clk(clk)");

    probe("ports/parameter-list-empty",
          R"(
    module param_mod #(parameter int A = 1, parameter int B = 2) (input logic clk);
    endmodule
    module params_top;
        logic clk;
        param_mod #(
        ) u_param(
        );
    endmodule
    )",
          atFrom("module params_top", "param_mod #("), "param_mod #(");

    probe("ports/parameter-dot",
          R"(
    module param_mod2 #(parameter int A = 1, parameter int B = 2) (input logic clk);
    endmodule
    module params_top2;
        logic clk;
        param_mod2 #(
            .
        ) u_param2(
        );
    endmodule
    )",
          atFrom("param_mod2 #(", "."), "param_mod2 #( .", ".");

    probe("instantiation/hash-with-params",
          R"(
    module hash_top;
        logic clk;
        Dut #
    endmodule
    )",
          at("Dut #"), "Dut #", "#");

    probe("instantiation/hash-without-params",
          R"(
    module hash_top2;
        logic clk;
        cycle_test_module #
    endmodule
    )",
          at("cycle_test_module #"), "cycle_test_module #", "#");

    probe("instantiation/library-module",
          R"(
    module lib_top;
        logic clk;
        logic q;
        Dut u_lib(.clk(clk));
    endmodule
    )",
          atFrom("Dut ", "u_lib"), "Dut u_lib");

    probe("instantiation/library-module-type",
          R"(
    module lib_top2;
        logic clk;
        Dut u_lib2();
    endmodule
    )",
          at("Dut u_li"), "Dut u_li");

    // -------------------------------------------------- scoped and member access
    constexpr std::string_view shapeMods = R"(
    package audit_pkg;
        typedef enum logic [1:0] {IDLE, RUN} state_e;
        localparam int DEPTH = 4;
        function automatic int add(int a, int b); return a + b; endfunction
        class Helper;
            int field;
        endclass
    endpackage

    interface audit_if2(input logic clk);
        logic valid;
        logic [7:0] payload;
        modport leader(output valid, output payload, input clk);
    endinterface

    typedef struct packed {
        logic [3:0] a;
        logic [3:0] b;
    } audit_pair_t;
    )";

    probe("scoped/package",
          std::string(shapeMods) + R"(
    module scope1(input logic clk);
        import audit_pkg::*;
        logic [7:0] x;
        initial begin
            x = audit_pkg::;
        end
    endmodule
    )",
          atFrom("x = ", "audit_pkg::"), "audit_pkg::", ":");

    probe("scoped/package-mid-word",
          std::string(shapeMods) + R"(
    module scope2(input logic clk);
        logic [7:0] x;
        initial begin
            x = audit_pkg::DE;
        end
    endmodule
    )",
          atFrom("x = ", "audit_pkg::DE"), "audit_pkg::DE");

    probe("scoped/enum-value",
          std::string(shapeMods) + R"(
    module scope3(input logic clk);
        import audit_pkg::*;
        audit_pkg::state_e state;
        initial begin
            state = audit_pkg::I;
        end
    endmodule
    )",
          atFrom("state = ", "audit_pkg::I"), "audit_pkg::I");

    probe("member/struct",
          std::string(shapeMods) + R"(
    module member1;
        audit_pair_t pair;
        logic [7:0] x;
        initial begin
            x = pair.a;
        end
    endmodule
    )",
          atFrom("x = ", "pair."), "x = pair.");

    probe("member/interface",
          std::string(shapeMods) + R"(
    module member2(input logic clk);
        audit_if2 simple(clk);
        logic [7:0] x;
        initial begin
            x = simple.valid;
        end
    endmodule
    )",
          atFrom("x = ", "simple."), "x = simple.");

    probe("member/class",
          std::string(shapeMods) + R"(
    module member3;
        audit_pkg::Helper helper;
        initial begin
            helper = new();
            helper.field = 1;
        end
    endmodule
    )",
          atFrom("helper = new();", "helper."), "helper.");

    probe("enum/value-rhs",
          std::string(shapeMods) + R"(
    module enum1(input logic clk);
        import audit_pkg::*;
        audit_pkg::state_e state;
        initial begin
            state = IDLE;
        end
    endmodule
    )",
          atFrom("state = ", "IDLE"), "state = IDLE");

    probe("assignment-pattern/open",
          std::string(shapeMods) + R"(
    module pattern1;
        audit_pair_t pair;
        initial begin
            pair = '{a: 4'h1, b: 4'h2};
        end
    endmodule
    )",
          atFrom("pair = ", "'{"), "pair = '{", "{");

    probe("assignment-pattern/key",
          std::string(shapeMods) + R"(
    module pattern2;
        audit_pair_t pair;
        initial begin
            pair = '{a: 4'h1, b: 4'h2};
        end
    endmodule
    )",
          atFrom("pair = ", "'{a"), "'{a");

    probe("assignment-pattern/key-colon",
          std::string(shapeMods) + R"(
    module pattern3;
        audit_pair_t pair;
        initial begin
            pair = '{a: 4'h1, b: 4'h2};
        end
    endmodule
    )",
          atFrom("pair = ", "'{a"), "'{a", ":");

    probe("assignment-pattern/value",
          std::string(shapeMods) + R"(
    module pattern4;
        audit_pair_t pair;
        initial begin
            pair = '{a: , b: 4'h2};
        end
    endmodule
    )",
          atFrom("pair = ", "'{a: "), "'{a: ");

    probe("call/arguments",
          std::string(shapeMods) + R"(
    module call1(input logic clk);
        import audit_pkg::*;
        logic [7:0] x;
        initial begin
            x = audit_pkg::add(1, );
        end
    endmodule
    )",
          atFrom("x = ", "audit_pkg::add(1, "), "audit_pkg::add(1, ");

    // ------------------------------------------------------- macros and system
    probe("macro/backtick",
          R"(
    `include "some_header.svh"
    `define AUDIT_MACRO(a, b) ((a) + (b))
    `define AUDIT_OTHER 1

    module macro1;
        logic [7:0] x;
        initial begin
            x = `AUDIT_OTHER;
        end
    endmodule
    )",
          atFrom("x = ", "`"), "x = `", "`");

    probe("macro/name-mid-word",
          R"(
    `define AUDIT_MACRO2(a, b) ((a) + (b))

    module macro2;
        logic [7:0] x;
        initial begin
            x = `AUDIT_MACRO2(1, 2);
        end
    endmodule
    )",
          atFrom("x = ", "`AUDIT_"), "x = `AUDIT_");

    probe("macro/arguments",
          R"(
    `define AUDIT_MACRO3(a, b) ((a) + (b))

    module macro3;
        logic [7:0] x;
        initial begin
            x = `AUDIT_MACRO3(1, );
        end
    endmodule
    )",
          atFrom("x = ", "`AUDIT_MACRO3(1, "), "x = `AUDIT_MACRO3(1, ");

    probe("system/trigger",
          R"(
    module sys1;
        logic [7:0] x;
        initial begin
            $display("x = %0d", x);
        end
    endmodule
    )",
          atFrom("initial begin", "$"), "$", "$");

    probe("system/name-mid-word",
          R"(
    module sys2;
        logic [7:0] x;
        initial begin
            $display("x = %0d", x);
        end
    endmodule
    )",
          atFrom("initial begin", "$dis"), "$dis");

    probe("system/arguments",
          R"(
    module sys3;
        logic [7:0] x;
        initial begin
            $display("x = %0d", );
        end
    endmodule
    )",
          atFrom("initial begin", "$display(\"x = %0d\", "), "$display(\"x = %0d\", ");

    // ------------------------------------------------------------- subroutines
    probe("subroutine/argument-name",
          R"(
    module routine1;
        function automatic void do_thing(int arg_one, int arg_two);
        endfunction
    endmodule
    )",
          at("int arg_on"), "int arg_on");

    probe("subroutine/argument-type",
          R"(
    module routine2;
        function automatic void do_thing(int arg_one);
        endfunction
    endmodule
    )",
          atFrom("void do_thing(", "int "), "do_thing(int ");

    probe("subroutine/call",
          R"(
    module routine3;
        function automatic void do_thing(int arg_one, int arg_two);
        endfunction
        initial begin
            do_thing(1, );
        end
    endmodule
    )",
          atFrom("initial begin", "do_thing(1, "), "do_thing(1, ");

    probe("case/item",
          R"(
    module case1;
        typedef enum logic {A, B} e_t;
        e_t e;
        always_comb begin
            case (e)
                A: e = B;
                
                default: e = A;
            endcase
        end
    endmodule
    )",
          atFrom("A: e = B;", "\n"), "case (e) statement",
          std::nullopt,
          // A case item body is a statement position, so an item there is the start of a statement
          {"ExpectedToken", "ExpectedExpression"});

    probe("class/member",
          R"(
    class Packet;
        rand int header;
        int len;
        function new();
        endfunction
        function int size();
            return len;
        endfunction
    endclass

    module class1;
        Packet pkt;
        initial begin
            pkt = new();
            pkt.header = 1;
        end
    endmodule
    )",
          atFrom("pkt = new();", "pkt."), "pkt.");

    probe("class/new",
          R"(
    class Packet2;
        int len;
    endclass

    module class2;
        Packet2 pkt2;
        initial begin
            pkt2 = new();
        end
    endmodule
    )",
          atFrom("pkt2 = ", "new"), "pkt2 = new");

    if (!findings.empty() || !duplicates.empty()) {
        std::string report;
        for (auto& summary : summaries) {
            report += "  " + summary + "\n";
        }
        for (auto& [key, entry] : findings) {
            report += fmt::format("  [{}x] {} {}\n", entry.first, key, entry.second);
        }
        for (auto& [key, labels] : duplicates) {
            report += fmt::format("  {} duplicate labels: {}\n", key, labels);
        }
        WARN(fmt::format("{} completions checked over {} positions, {} problem groups, {} positions "
                         "with duplicate labels:\n{}",
                         checked, summaries.size(), findings.size(), duplicates.size(), report));
    }
    CHECK(findings.empty());
    CHECK(duplicates.empty());
    CHECK(checked > 0);
}
