#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "Protocol.h"
#include "ProjectConfig.h"

// Forward declarations from SafeC frontend
namespace safec {
    struct TranslationUnit;
}

namespace lsp {

// ── SymbolInfo ───────────────────────────────────────────────────────────────

enum class SymKind { Function, Method, Struct, Enum, GlobalVar, TypeAlias, Newtype, Region };

struct SymbolInfo {
    std::string name;
    std::string typeStr;   // human-readable type, e.g. "int(int, int)"
    SymKind     kind       = SymKind::GlobalVar;
    Position    pos;       // 0-based line/col of declaration
    std::string docUri;    // document URI this symbol belongs to
    std::string owner;     // a method's struct ("Vec" for Vec::push), else ""
    bool        hasBody = false;   // a function definition, not a declaration
};

// ── AnalysisResult ───────────────────────────────────────────────────────────

struct AnalysisResult {
    std::string             uri;          // the analyzed document
    std::vector<Diagnostic> diagnostics;
    std::vector<SymbolInfo> symbols;
    // TU pointer is kept alive so hover/definition can walk it
    std::shared_ptr<safec::TranslationUnit> tu;
};

// ── Public API ───────────────────────────────────────────────────────────────

// Run the full SafeC frontend (Preprocessor→Lexer→Parser→Sema) on `source`.
// `uri` is used to construct definition Locations.
// `opts` carries what the safec driver would get from its command line.
AnalysisResult analyze(const std::string &uri,
                       const std::string &source,
                       const AnalysisOptions &opts = {});

// Same as analyze(), for a .scx buffer: transpiles `scxSource` (safeguard's
// ScxTranspiler — see ScxTranspiler.h) to plain SafeC, runs analyze() on
// the generated text, then remaps every diagnostic/symbol line number in
// the result back to `scxSource`'s own line numbers using the transpiler's
// line map, so diagnostics/hover/go-to-def/document-symbols all land in
// the buffer the client actually has open rather than in generated code it
// never sees. Column numbers are passed through unchanged (see
// ScxTranspiler.h's line-map contract for why line-level mapping is the
// right precision here — the whole file besides markup expansions is a
// byte-for-byte copy, so columns already line up outside those spans).
// If the markup itself is malformed, the transpiler's own "<file>:<line>:
// scx: <msg>" error becomes a single diagnostic at that line.
AnalysisResult analyzeScx(const std::string &uri,
                          const std::string &scxSource,
                          const AnalysisOptions &opts = {});

// Return hover markdown for the symbol at (line, col) (0-based).
std::optional<Hover> hover(const AnalysisResult &result,
                           const std::string &source,
                           int line, int col);

// Return completion items (all symbols + SafeC keywords).
std::vector<CompletionItem> complete(const AnalysisResult &result);

// Return definition location for the symbol at (line, col) (0-based).
std::optional<Location> definition(const AnalysisResult &result,
                                   const std::string &source,
                                   const std::string &uri,
                                   int line, int col);

// Return document symbols for outline view.
std::vector<DocumentSymbol> documentSymbols(const AnalysisResult &result);

// Extract the identifier word at (line, col) from source text (0-based).
std::string wordAt(const std::string &source, int line, int col);

} // namespace lsp
