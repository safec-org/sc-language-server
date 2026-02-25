#pragma once
#include <string>
#include <vector>
#include <optional>
#include "nlohmann/json.hpp"

namespace lsp {

using json = nlohmann::json;

// ── Basic LSP types ──────────────────────────────────────────────────────────

struct Position {
    int line      = 0; // 0-based
    int character = 0; // 0-based

    json toJson() const { return {{"line", line}, {"character", character}}; }
    static Position fromJson(const json &j) {
        return {j.at("line").get<int>(), j.at("character").get<int>()};
    }
};

struct Range {
    Position start;
    Position end;

    json toJson() const { return {{"start", start.toJson()}, {"end", end.toJson()}}; }
    static Range fromJson(const json &j) {
        return {Position::fromJson(j.at("start")), Position::fromJson(j.at("end"))};
    }
    static Range point(int line, int col) {
        Position p{line, col};
        return {p, p};
    }
};

struct Location {
    std::string uri;
    Range       range;

    json toJson() const { return {{"uri", uri}, {"range", range.toJson()}}; }
};

// ── Diagnostic ───────────────────────────────────────────────────────────────

enum class DiagnosticSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct Diagnostic {
    Range             range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string       message;
    std::string       source = "safec";

    json toJson() const {
        return {
            {"range",    range.toJson()},
            {"severity", static_cast<int>(severity)},
            {"message",  message},
            {"source",   source}
        };
    }
};

// ── Completion ───────────────────────────────────────────────────────────────

enum class CompletionItemKind {
    Text = 1, Method = 2, Function = 3, Constructor = 4,
    Field = 5, Variable = 6, Class = 7, Interface = 8,
    Module = 9, Property = 10, Unit = 11, Value = 12,
    Enum = 13, Keyword = 14, Snippet = 15, Color = 16,
    File = 17, Reference = 18, Folder = 19, EnumMember = 20,
    Constant = 21, Struct = 22, Event = 23, Operator = 24,
    TypeParameter = 25
};

struct CompletionItem {
    std::string        label;
    CompletionItemKind kind    = CompletionItemKind::Text;
    std::string        detail;
    std::string        documentation;

    json toJson() const {
        json j = {{"label", label}, {"kind", static_cast<int>(kind)}};
        if (!detail.empty())        j["detail"]        = detail;
        if (!documentation.empty()) j["documentation"] = documentation;
        return j;
    }
};

// ── Document Symbol ──────────────────────────────────────────────────────────

enum class SymbolKind {
    File = 1, Module = 2, Namespace = 3, Package = 4, Class = 5,
    Method = 6, Property = 7, Field = 8, Constructor = 9, Enum = 10,
    Interface = 11, Function = 12, Variable = 13, Constant = 14,
    String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19,
    Key = 20, Null = 21, EnumMember = 22, Struct = 23, Event = 24,
    Operator = 25, TypeParameter = 26
};

struct DocumentSymbol {
    std::string name;
    SymbolKind  kind  = SymbolKind::Variable;
    Range       range;
    Range       selectionRange;

    json toJson() const {
        return {
            {"name",            name},
            {"kind",            static_cast<int>(kind)},
            {"range",           range.toJson()},
            {"selectionRange",  selectionRange.toJson()}
        };
    }
};

// ── Hover ────────────────────────────────────────────────────────────────────

struct Hover {
    std::string contents; // MarkupContent value (markdown)
    Range       range;

    json toJson() const {
        return {
            {"contents", {{"kind", "markdown"}, {"value", contents}}},
            {"range",    range.toJson()}
        };
    }
};

} // namespace lsp
