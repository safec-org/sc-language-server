#include "Analysis.h"
#include "safec/Diagnostic.h"
#include "safec/Preprocessor.h"
#include "safec/Lexer.h"
#include "safec/Parser.h"
#include "safec/Sema.h"
#include "safec/ConstEval.h"
#include "safec/AST.h"
#include "safec/Type.h"
#include <sstream>
#include <cctype>
#include <algorithm>

namespace lsp {

// ── SafeC keyword list (for completion) ─────────────────────────────────────
static const char *kKeywords[] = {
    // C keywords
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "bool", "true", "false", "null",
    // SafeC extension keywords
    "region", "unsafe", "consteval", "constinit", "generic", "static_assert",
    "self", "operator", "new", "arena_reset", "arena_destroy", "arena_mark",
    "arena_free_to", "tuple", "namespace",
    "spawn", "join", "defer", "errdefer", "match", "packed", "try",
    "must_use", "fn", "alignof", "typeof", "fieldcount",
    "trait", "fn_eval",
    // Nullable-pointer / nullable-reference / optional (?T) vocabulary:
    // 'some'/'none' are match-pattern identifiers (not general-expression
    // constructors — see reference/types.md), and is_null()/is_none() are
    // the safe presence checks required (alongside match/.default(...) or
    // an explicit 'unsafe' block) before reading one of these types. None
    // of these are reserved words in the grammar — they're resolved
    // contextually by Sema against the receiver's type — but are listed
    // here so they still surface in completion.
    "is_null", "is_none", "some", "none",
    // C11 (superset completeness)
    "_Generic",
    // Bare-metal / effect system keywords
    "naked", "interrupt", "section", "noreturn", "asm", "pure",
    "atomic", "newtype", "align", "thread_local",
    "__stdcall", "__cdecl", "__fastcall",
    // Types — sized aliases keep the '_t' suffix (see
    // Sema::registerBuiltinTypes in the compiler): there is no bare
    // 'int32'/'uint32'/etc. type, and no 'float32'/'float64' at all (use
    // 'float'/'double', already listed above under C keywords).
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    // vec<T,N> — native SIMD vector type (contextual, parsed like a
    // generic type name rather than a reserved word, but listed here so
    // it still shows up in completion)
    "vec",
    // Alternate spellings
    "_Thread_local", "__thread",
    // Region qualifiers (contextual)
    "stack", "heap", "arena", "capacity",
    // Built-in functions
    "volatile_load", "volatile_store",
    "atomic_load", "atomic_store", "atomic_fetch_add", "atomic_fetch_sub",
    "atomic_fetch_and", "atomic_fetch_or", "atomic_fetch_xor",
    "atomic_exchange", "atomic_cas", "atomic_compare_exchange_strong",
    "atomic_fence",
    "chan_create", "chan_send", "chan_recv", "chan_close",
    "spawn_scoped",
    // malloc_defer(n) isn't a real callable (no declared symbol anywhere —
    // it's pure parser sugar recognized only as a var-decl initializer,
    // expanding to alloc(n) + an injected 'defer dealloc(...)' at parse
    // time), so it never surfaces via the live Sema-symbol completion path
    // the way a real function would; listed here so it still autocompletes.
    "malloc_defer",
    // GCC/Clang-style bit-manipulation built-ins
    "__builtin_popcount", "__builtin_popcountll",
    "__builtin_clz", "__builtin_clzll",
    "__builtin_ctz", "__builtin_ctzll",
    "__builtin_bswap32", "__builtin_bswap64",
    // ARM Cortex-M4/M7 DSP-extension built-ins (ARM target only)
    "__arm_dsp_qadd", "__arm_dsp_qsub",
    "__arm_dsp_qadd16", "__arm_dsp_qadd8", "__arm_dsp_qsub16", "__arm_dsp_qsub8",
    "__arm_dsp_sadd16", "__arm_dsp_sadd8", "__arm_dsp_ssub16", "__arm_dsp_ssub8",
    "__arm_dsp_uqadd16", "__arm_dsp_uqadd8", "__arm_dsp_uqsub16", "__arm_dsp_uqsub8",
    "__arm_dsp_smlad", "__arm_dsp_smladx", "__arm_dsp_smlsd", "__arm_dsp_smlsdx",
    "__arm_dsp_smuad", "__arm_dsp_smuadx", "__arm_dsp_smusd", "__arm_dsp_smusdx",
    "__arm_dsp_usad8", "__arm_dsp_usada8",
    "__arm_dsp_ssat", "__arm_dsp_usat", "__arm_dsp_ssat16", "__arm_dsp_usat16",
    "__arm_dsp_sxtab16", "__arm_dsp_uxtab16",
    // Compound keywords
    "if const", nullptr
};

// ── wordAt ────────────────────────────────────────────────────────────────────
std::string wordAt(const std::string &source, int line, int col) {
    // Split into lines (0-based)
    size_t lineStart = 0;
    int    curLine   = 0;
    while (curLine < line) {
        size_t nl = source.find('\n', lineStart);
        if (nl == std::string::npos) return "";
        lineStart = nl + 1;
        ++curLine;
    }
    // Find end of line
    size_t lineEnd = source.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = source.size();

    std::string lineStr = source.substr(lineStart, lineEnd - lineStart);
    if (col < 0 || col >= (int)lineStr.size()) return "";

    // Scan back to start of identifier
    int start = col;
    while (start > 0 && (std::isalnum((unsigned char)lineStr[start - 1]) ||
                          lineStr[start - 1] == '_'))
        --start;
    // Scan forward to end of identifier
    int end = col;
    while (end < (int)lineStr.size() && (std::isalnum((unsigned char)lineStr[end]) ||
                                          lineStr[end] == '_'))
        ++end;

    if (start == end) return "";
    return lineStr.substr(start, end - start);
}

// ── analyze ───────────────────────────────────────────────────────────────────
AnalysisResult analyze(const std::string &uri,
                       const std::string &source,
                       const std::vector<std::string> &includeDirs) {
    AnalysisResult result;

    // Extract a simple filename from uri for diagnostics
    std::string filename = uri;
    auto slash = uri.rfind('/');
    if (slash != std::string::npos) filename = uri.substr(slash + 1);
    // Strip leading "file://" if present
    if (filename.rfind("file://", 0) == 0) filename = filename.substr(7);

    safec::DiagEngine diag(filename.c_str());
    diag.setSilent(true); // suppress stderr during IDE analysis

    // ── Preprocessor ──────────────────────────────────────────────────────────
    safec::PreprocOptions popts;
    popts.importCHeaders = false; // skip slow clang import in LSP mode
    popts.includePaths   = includeDirs;

    safec::Preprocessor pp(source, filename, diag, popts);
    std::string preprocessed = pp.process();

    if (diag.hasErrors()) {
        goto convert_diags;
    }

    {
        // ── Lexer ──────────────────────────────────────────────────────────────
        safec::Lexer lexer(preprocessed, filename.c_str(), diag);
        auto tokens = lexer.lexAll();

        if (diag.hasErrors()) goto convert_diags;

        // ── Parser ─────────────────────────────────────────────────────────────
        safec::Parser parser(std::move(tokens), diag);
        auto tuOwned = parser.parseTranslationUnit();

        if (!tuOwned || diag.hasErrors()) goto convert_diags;

        // ── Sema ───────────────────────────────────────────────────────────────
        safec::Sema sema(*tuOwned, diag);
        sema.run(); // errors recorded but don't abort

        // ── Build SymbolInfo from TU decls ─────────────────────────────────────
        for (auto &declPtr : tuOwned->decls) {
            if (!declPtr) continue;
            safec::Decl *d = declPtr.get();

            SymbolInfo sym;
            sym.name   = d->name;
            sym.docUri = uri;
            // Convert 1-based SafeC loc → 0-based LSP position
            sym.pos.line      = static_cast<int>(d->loc.line)   - 1;
            sym.pos.character = static_cast<int>(d->loc.col)    - 1;
            if (sym.pos.line      < 0) sym.pos.line      = 0;
            if (sym.pos.character < 0) sym.pos.character = 0;

            switch (d->kind) {
            case safec::DeclKind::Function: {
                auto *fn = static_cast<safec::FunctionDecl *>(d);
                sym.kind = SymKind::Function;
                if (fn->returnType)
                    sym.typeStr = fn->returnType->str() + " " + fn->name + "(...)";
                else
                    sym.typeStr = fn->name + "(...)";
                // Skip monomorphized generics (mangled names with __)
                if (fn->name.size() >= 2 && fn->name[0] == '_' && fn->name[1] == '_')
                    continue;
                break;
            }
            case safec::DeclKind::Struct:
                sym.kind    = SymKind::Struct;
                sym.typeStr = "struct " + d->name;
                break;
            case safec::DeclKind::Enum:
                sym.kind    = SymKind::Enum;
                sym.typeStr = "enum " + d->name;
                break;
            case safec::DeclKind::GlobalVar: {
                auto *gv = static_cast<safec::GlobalVarDecl *>(d);
                sym.kind    = SymKind::GlobalVar;
                sym.typeStr = (gv->type ? gv->type->str() : "?") + " " + d->name;
                break;
            }
            case safec::DeclKind::TypeAlias:
                sym.kind    = SymKind::TypeAlias;
                sym.typeStr = "typedef " + d->name;
                break;
            case safec::DeclKind::Newtype:
                sym.kind    = SymKind::Newtype;
                sym.typeStr = "newtype " + d->name;
                break;
            case safec::DeclKind::Region:
                sym.kind    = SymKind::Region;
                sym.typeStr = "region " + d->name;
                break;
            default:
                continue; // skip StaticAssert
            }
            if (!sym.name.empty())
                result.symbols.push_back(std::move(sym));
        }

        result.tu = std::shared_ptr<safec::TranslationUnit>(tuOwned.release());
    }

convert_diags:
    // ── Convert DiagEngine diagnostics → LSP Diagnostics ──────────────────────
    for (auto &d : diag.diagnostics()) {
        Diagnostic lspD;
        // loc is 1-based; LSP wants 0-based
        int ln  = static_cast<int>(d.loc.line) - 1;
        int col = static_cast<int>(d.loc.col)  - 1;
        if (ln  < 0) ln  = 0;
        if (col < 0) col = 0;

        lspD.range.start = {ln, col};
        lspD.range.end   = {ln, col + 1};
        lspD.message     = d.message;
        switch (d.level) {
        case safec::DiagLevel::Note:
            lspD.severity = DiagnosticSeverity::Information; break;
        case safec::DiagLevel::Warning:
            lspD.severity = DiagnosticSeverity::Warning; break;
        case safec::DiagLevel::Error:
        case safec::DiagLevel::Fatal:
            lspD.severity = DiagnosticSeverity::Error; break;
        }
        result.diagnostics.push_back(std::move(lspD));
    }

    return result;
}

// ── hover ─────────────────────────────────────────────────────────────────────
std::optional<Hover> hover(const AnalysisResult &result,
                           const std::string &source,
                           int line, int col) {
    std::string word = wordAt(source, line, col);
    if (word.empty()) return std::nullopt;

    for (auto &sym : result.symbols) {
        if (sym.name == word) {
            Hover h;
            h.contents = "```safec\n" + sym.typeStr + "\n```";
            h.range    = Range::point(sym.pos.line, sym.pos.character);
            return h;
        }
    }
    return std::nullopt;
}

// ── complete ──────────────────────────────────────────────────────────────────
std::vector<CompletionItem> complete(const AnalysisResult &result) {
    std::vector<CompletionItem> items;

    // All top-level symbols
    for (auto &sym : result.symbols) {
        CompletionItem item;
        item.label  = sym.name;
        item.detail = sym.typeStr;
        switch (sym.kind) {
        case SymKind::Function:  item.kind = CompletionItemKind::Function; break;
        case SymKind::Struct:    item.kind = CompletionItemKind::Struct;   break;
        case SymKind::Enum:      item.kind = CompletionItemKind::Enum;     break;
        case SymKind::GlobalVar: item.kind = CompletionItemKind::Variable; break;
        case SymKind::TypeAlias: item.kind = CompletionItemKind::Class;    break;
        case SymKind::Newtype:   item.kind = CompletionItemKind::Class;    break;
        case SymKind::Region:    item.kind = CompletionItemKind::Module;   break;
        }
        items.push_back(std::move(item));
    }

    // SafeC keywords
    for (const char **kw = kKeywords; *kw; ++kw) {
        CompletionItem item;
        item.label = *kw;
        item.kind  = CompletionItemKind::Keyword;
        items.push_back(std::move(item));
    }

    return items;
}

// ── definition ────────────────────────────────────────────────────────────────
std::optional<Location> definition(const AnalysisResult &result,
                                   const std::string &source,
                                   const std::string &uri,
                                   int line, int col) {
    std::string word = wordAt(source, line, col);
    if (word.empty()) return std::nullopt;

    for (auto &sym : result.symbols) {
        if (sym.name == word) {
            Location loc;
            loc.uri   = sym.docUri.empty() ? uri : sym.docUri;
            loc.range = Range::point(sym.pos.line, sym.pos.character);
            return loc;
        }
    }
    return std::nullopt;
}

// ── documentSymbols ──────────────────────────────────────────────────────────
std::vector<DocumentSymbol> documentSymbols(const AnalysisResult &result) {
    std::vector<DocumentSymbol> syms;
    for (auto &s : result.symbols) {
        DocumentSymbol ds;
        ds.name  = s.name;
        ds.range = Range::point(s.pos.line, s.pos.character);
        ds.selectionRange = ds.range;
        switch (s.kind) {
        case SymKind::Function:  ds.kind = SymbolKind::Function;  break;
        case SymKind::Struct:    ds.kind = SymbolKind::Struct;     break;
        case SymKind::Enum:      ds.kind = SymbolKind::Enum;       break;
        case SymKind::GlobalVar: ds.kind = SymbolKind::Variable;   break;
        case SymKind::TypeAlias: ds.kind = SymbolKind::Class;      break;
        case SymKind::Newtype:   ds.kind = SymbolKind::Class;      break;
        case SymKind::Region:    ds.kind = SymbolKind::Namespace;  break;
        }
        syms.push_back(std::move(ds));
    }
    return syms;
}

} // namespace lsp
