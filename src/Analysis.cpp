#include "Analysis.h"
#include "safec/Diagnostic.h"
#include "safec/Preprocessor.h"
#include "safec/Lexer.h"
#include "safec/Parser.h"
#include "safec/Sema.h"
#include "safec/ConstEval.h"
#include "safec/AST.h"
#include "safec/Type.h"
#include "ScxTranspiler.h"
#include "CHeaderBridge.h"
#include <sstream>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <set>

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
    // Ownership (Drop) and thread-safety (Send/Sync) vocabulary, all
    // contextual: 'R m() consume;' methods, drop(x) / forget(x) /
    // needs_drop(T), 'unsafe impl Send + Sync;' and 'impl !Send;' markers
    // in a struct body, and the marker traits themselves.
    "consume", "drop", "forget", "needs_drop", "impl", "Send", "Sync",
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
    // Floating-point built-ins (float, double, vec<float/double, N>)
    "__builtin_fmuladd", "__builtin_sqrt",
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

// Extract a simple filename from a uri (strip directory + "file://").
static std::string deriveFilename(const std::string &uri) {
    std::string filename = uri;
    auto slash = uri.rfind('/');
    if (slash != std::string::npos) filename = uri.substr(slash + 1);
    if (filename.rfind("file://", 0) == 0) filename = filename.substr(7);
    return filename;
}

// The name as written in source: Sema mangles 'namespace std { f }' to
// "std_f" and a method 'Vec::push' to "Vec_push"; strip those prefixes so
// hover/completion/definition match what the user typed.
static std::string sourceName(const safec::FunctionDecl &fn) {
    std::string n = fn.name;
    auto strip = [&](const std::string &prefix) {
        if (!prefix.empty() && n.size() > prefix.size() + 1 &&
            n.compare(0, prefix.size(), prefix) == 0 && n[prefix.size()] == '_')
            n = n.substr(prefix.size() + 1);
    };
    strip(fn.namespaceName);
    strip(fn.methodOwner);
    return n;
}

// Type::str() for display: like the compiler's, but keeps a generic
// struct's type arguments ("Vec<T>", not "Vec"), which the compiler's own
// str() leaves off because it doubles as a mangling key.
static std::string displayType(const safec::TypePtr &t) {
    if (!t) return "?";
    switch (t->kind) {
    case safec::TypeKind::Struct: {
        auto &st = static_cast<const safec::StructType &>(*t);
        if (st.typeArgs.empty()) return t->str();
        std::string s = "struct " + st.name + "<";
        for (size_t i = 0; i < st.typeArgs.size(); ++i)
            s += (i ? ", " : "") + displayType(st.typeArgs[i]);
        return s + ">";
    }
    case safec::TypeKind::Reference: {
        auto &rt = static_cast<const safec::ReferenceType &>(*t);
        if (rt.base && rt.base->kind == safec::TypeKind::Function) {
            // 'fn R(Args)' / '?fn R(Args)': how the source spells a
            // function reference (a '&static const' function underneath).
            auto &ft = static_cast<const safec::FunctionType &>(*rt.base);
            std::string s = std::string(rt.nullable ? "?fn " : "fn ") + displayType(ft.returnType) + "(";
            for (size_t i = 0; i < ft.paramTypes.size(); ++i)
                s += (i ? ", " : "") + displayType(ft.paramTypes[i]);
            if (ft.variadic) s += ft.paramTypes.empty() ? "..." : ", ...";
            return s + ")";
        }
        std::string s = rt.nullable ? "?&" : "&";
        if (rt.region == safec::Region::Arena) s += "arena<" + rt.arenaName + "> ";
        else if (rt.region != safec::Region::Extern) s += safec::regionName(rt.region) + " ";
        if (!rt.mut) s += "const ";
        if (rt.pointeeVolatile) s += "volatile ";
        return s + displayType(rt.base);
    }
    case safec::TypeKind::Slice: {
        auto &sl = static_cast<const safec::SliceType &>(*t);
        return (sl.constElem ? "[]const " : "[]") + displayType(sl.element);
    }
    case safec::TypeKind::Optional:
        return "?" + displayType(static_cast<const safec::OptionalType &>(*t).inner);
    case safec::TypeKind::Pointer: {
        auto &pt = static_cast<const safec::PointerType &>(*t);
        return std::string(pt.isConst ? "const " : "") + (pt.pointeeVolatile ? "volatile " : "") +
               displayType(pt.base) + "*";
    }
    default:
        return t->str();
    }
}

static std::string genericPrefix(const std::vector<safec::GenericParam> &gp) {
    if (gp.empty()) return "";
    std::string s = "generic<";
    for (size_t i = 0; i < gp.size(); ++i) {
        s += (i ? ", " : "") + gp[i].name;
        for (size_t k = 0; k < gp[i].constraints.size(); ++k)
            s += (k ? " + " : ": ") + gp[i].constraints[k];
    }
    return s + "> ";
}

// e.g. "generic<T> struct Vec<T> std::vec_with_cap(unsigned long cap)".
static std::string signature(const safec::FunctionDecl &fn) {
    std::string sig = genericPrefix(fn.genericParams);
    sig += (fn.returnType ? displayType(fn.returnType) : std::string("void")) + " ";
    if (!fn.namespaceName.empty()) sig += fn.namespaceName + "::";
    if (!fn.methodOwner.empty()) sig += fn.methodOwner + "::";
    sig += sourceName(fn) + "(";
    bool first = true;
    for (const auto &p : fn.params) {
        if (p.name == "self") continue;   // implicit method receiver
        if (!first) sig += ", ";
        first = false;
        sig += displayType(p.type);
        if (!p.name.empty()) sig += " " + p.name;
    }
    if (fn.isVariadic) sig += first ? "..." : ", ...";
    sig += ")";
    if (fn.isConstMethod) sig += " const";
    if (fn.isConsume && sourceName(fn) != "drop") sig += " consume";
    return sig;
}

// e.g. "generic<T> struct Vec<T>", plus its Send/Sync markers.
static std::string structSignature(const safec::StructDecl &sd) {
    std::string s = genericPrefix(sd.genericParams) + (sd.isUnion ? "union " : "struct ") + sd.name;
    if (!sd.genericParams.empty()) {
        s += "<";
        for (size_t i = 0; i < sd.genericParams.size(); ++i)
            s += (i ? ", " : "") + sd.genericParams[i].name;
        s += ">";
    }
    for (const auto &m : sd.threadMarkers)
        s += m[0] == '!' ? "\n    impl " + m + ";" : "\n    unsafe impl " + m + ";";
    return s;
}

// ── analyze ───────────────────────────────────────────────────────────────────
// A location belongs to the open document when the line markers left it
// on the document's own file; anything else came from an #include'd file.
static bool inThisFile(const safec::SourceLocation &loc, const std::string &filename) {
    return !loc.file || filename == loc.file;
}

static std::string fileUri(const std::string &path) {
    return path.rfind("file://", 0) == 0 ? path : "file://" + path;
}

// The name the document is preprocessed and lexed under: its full path, so
// a quoted '#include "x.h"' is found next to it as safec finds it. Interned
// for the process lifetime because SourceLocation keeps a raw pointer to
// it (Sema/MemoryFlow diagnostics, the kept TU).
static const std::string &internFilename(const std::string &uri) {
    static std::set<std::string> names;
    std::string name = uri.rfind("file://", 0) == 0 ? uriToPath(uri) : deriveFilename(uri);
    return *names.insert(name).first;
}

// The end column (0-based, exclusive) of the token starting at 'col' on
// 'line' of 'source': an identifier/number spans its characters, anything
// else one character.
static int tokenEnd(const std::string &source, int line, int col) {
    size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        pos = source.find('\n', pos);
        if (pos == std::string::npos) return col + 1;
        ++pos;
    }
    size_t eol = source.find('\n', pos);
    if (eol == std::string::npos) eol = source.size();
    size_t i = pos + (size_t)col;
    if (i >= eol) return col + 1;
    auto word = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    if (!word(source[i])) return col + 1;
    size_t j = i;
    while (j < eol && word(source[j])) ++j;
    return col + (int)(j - i);
}

AnalysisResult analyze(const std::string &uri,
                       const std::string &source,
                       const AnalysisOptions &opts) {
    AnalysisResult result;
    result.uri = uri;

    const std::string &filename = internFilename(uri);

    safec::DiagEngine diag(filename.c_str());
    diag.setSilent(true); // suppress stderr during IDE analysis

    // ── Preprocessor ──────────────────────────────────────────────────────────
    // Same settings the safec driver derives from its command line (see
    // ProjectConfig.h). C headers (<stdio.h>, <math.h>: libc's, since only
    // std's parent directory is on the include path) are imported the way
    // safec imports them, by asking safec (CHeaderBridge.h; cached).
    configureCHeaderImport(findSafec(), opts.target);
    safec::PreprocOptions popts;
    popts.importCHeaders = true;
    popts.includePaths   = opts.includeDirs;
    popts.compatMode     = opts.compat;
    popts.freestanding   = opts.freestanding;
    for (const auto &d : opts.defines) popts.cmdlineDefs[d.first] = d.second;

    // The preprocessor's diagnostics inside an #include'd file point at
    // that file's name in a string that is gone once the include has been
    // processed (Preprocessor::handleInclude's local 'path'); only the
    // document's own name — the preprocessor's 'filename_', which it takes
    // over (moves) from the string passed in here — stays valid. So its
    // address is noted to tell the two apart without reading freed memory.
    // A heap-allocated string keeps its buffer across a move; one short
    // enough for the small-string optimization is padded with "./".
    std::string ppName = filename;
    if (ppName.size() < 32) {
        auto slash = ppName.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "" : ppName.substr(0, slash + 1);
        std::string base = slash == std::string::npos ? ppName : ppName.substr(slash + 1);
        while (dir.size() + base.size() < 32) dir += "./";
        ppName = dir + base;
    }
    const char *ppNamePtr = ppName.data();
    safec::Preprocessor pp(source, std::move(ppName), diag, popts);
    std::string preprocessed = pp.process();
    const size_t ppDiagCount = diag.diagnostics().size();

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

        // ── Array sizes (const/consteval), as the driver does before Sema ──
        {
            safec::ConstEvalEngine arraySizeCE(*tuOwned, diag);
            if (!arraySizeCE.resolveArraySizes() || diag.hasErrors()) goto convert_diags;
        }

        // ── Sema ───────────────────────────────────────────────────────────────
        safec::Sema sema(*tuOwned, diag);
        sema.setFreestanding(opts.freestanding);
        sema.setPointerBits(opts.pointerBits);
        for (const auto &n : opts.provides) sema.addProvidedFunction(n);
        sema.run(); // errors recorded but don't abort

        // ── Build SymbolInfo from TU decls ─────────────────────────────────────
        for (auto &declPtr : tuOwned->decls) {
            if (!declPtr) continue;
            safec::Decl *d = declPtr.get();

            SymbolInfo sym;
            sym.name   = d->name;
            // A declaration from an #include'd header points at that header.
            sym.docUri = inThisFile(d->loc, filename) ? uri : fileUri(d->loc.file);
            // Convert 1-based SafeC loc → 0-based LSP position
            sym.pos.line      = static_cast<int>(d->loc.line)   - 1;
            sym.pos.character = static_cast<int>(d->loc.col)    - 1;
            if (sym.pos.line      < 0) sym.pos.line      = 0;
            if (sym.pos.character < 0) sym.pos.character = 0;

            switch (d->kind) {
            case safec::DeclKind::Function: {
                auto *fn = static_cast<safec::FunctionDecl *>(d);
                sym.kind = SymKind::Function;
                // Skip monomorphized generics (mangled names with __)
                if (fn->name.size() >= 2 && fn->name[0] == '_' && fn->name[1] == '_')
                    continue;
                sym.name    = sourceName(*fn);
                sym.typeStr = signature(*fn);
                if (fn->isMethod) {
                    sym.kind  = SymKind::Method;
                    sym.owner = fn->methodOwner;
                }
                sym.hasBody = fn->body != nullptr;
                break;
            }
            case safec::DeclKind::Struct:
                sym.kind    = SymKind::Struct;
                sym.typeStr = structSignature(*static_cast<safec::StructDecl *>(d));
                break;
            case safec::DeclKind::Enum:
                sym.kind    = SymKind::Enum;
                sym.typeStr = "enum " + d->name;
                break;
            case safec::DeclKind::GlobalVar: {
                auto *gv = static_cast<safec::GlobalVarDecl *>(d);
                sym.kind    = SymKind::GlobalVar;
                sym.typeStr = std::string(gv->isVolatile ? "volatile " : "") +
                              displayType(gv->type) + " " + d->name;
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
    // A note ("'b' declared here", "'b' was dropped here") explains the
    // error or warning before it: it becomes that diagnostic's
    // relatedInformation, and also a Hint at its own place pointing back.
    const auto &all = diag.diagnostics();
    int primary = -1;   // index in result.diagnostics of the last error/warning
    Location primaryLoc;
    for (size_t i = 0; i < all.size(); ++i) {
        const auto &d = all[i];
        // loc is 1-based; LSP wants 0-based
        int ln  = static_cast<int>(d.loc.line) - 1;
        int col = static_cast<int>(d.loc.col)  - 1;
        if (ln  < 0) ln  = 0;
        if (col < 0) col = 0;

        // Where it is: this document, a named header, or (a preprocessor
        // diagnostic in an #include'd file, see above) an unnamed one.
        bool here, named = true;
        if (i < ppDiagCount && d.loc.file && d.loc.file != ppNamePtr) {
            here = false;
            named = false;
        } else {
            here = !d.loc.file || d.loc.file == ppNamePtr || inThisFile(d.loc, filename);
        }

        // Its real place (for relatedInformation), and where it is shown:
        // one raised inside an #include'd header goes at the top of this
        // document, naming the header, since the header's line numbers
        // mean nothing here.
        Location real;
        Range top{{0, 0}, {0, 1}};
        std::string message = d.message;
        if (here) {
            real = {uri, {{ln, col}, {ln, tokenEnd(source, ln, col)}}};
        } else if (named) {
            real = {fileUri(d.loc.file), {{ln, col}, {ln, col + 1}}};
            message = std::string("in ") + d.loc.file + ":" + std::to_string(d.loc.line) + ": " + d.message;
        } else {
            real = {uri, top};
            message = "in an #include'd file, line " + std::to_string(d.loc.line) + ": " + d.message;
        }

        Diagnostic lspD;
        lspD.range   = here ? real.range : top;
        lspD.message = message;
        switch (d.level) {
        case safec::DiagLevel::Note:
            lspD.severity = DiagnosticSeverity::Information; break;
        case safec::DiagLevel::Warning:
            lspD.severity = DiagnosticSeverity::Warning; break;
        case safec::DiagLevel::Error:
        case safec::DiagLevel::Fatal:
            lspD.severity = DiagnosticSeverity::Error; break;
        }

        if (d.level == safec::DiagLevel::Note && primary >= 0) {
            Diagnostic &p = result.diagnostics[primary];
            p.related.push_back({real, d.message});
            if (!here) continue;   // a note inside a header: related info only
            lspD.severity = DiagnosticSeverity::Hint;
            lspD.related.push_back({primaryLoc, p.message});
        } else if (d.level != safec::DiagLevel::Note) {
            primary    = static_cast<int>(result.diagnostics.size());
            primaryLoc = real;
            if (!here && named) lspD.related.push_back({real, d.message});   // jump to the header
        }
        result.diagnostics.push_back(std::move(lspD));
    }

    return result;
}

// ── analyzeScx ────────────────────────────────────────────────────────────────
AnalysisResult analyzeScx(const std::string &uri,
                          const std::string &scxSource,
                          const AnalysisOptions &opts) {
    std::string filename = deriveFilename(uri);

    std::vector<int> lineMap; // ScxTranspiler's map: un-flattened generated
                              // line (pre-#include-expansion) -> orig .scx line
    std::string generated;
    try {
        generated = safeguard::transpileScx(scxSource, filename, &lineMap);
    } catch (std::exception &e) {
        // Malformed markup — the transpiler's own message is
        // "<filename>:<line>: scx: <detail>"; pull the line back out so
        // this still lands as a positioned diagnostic instead of a
        // generic line-1 error.
        AnalysisResult result;
        std::string msg = e.what();
        int errLine = 1;
        std::string prefix = filename + ":";
        if (msg.rfind(prefix, 0) == 0) {
            size_t p = prefix.size();
            size_t q = msg.find(':', p);
            if (q != std::string::npos) {
                try { errLine = std::stoi(msg.substr(p, q - p)); } catch (...) {}
            }
        }
        Diagnostic d;
        d.range    = Range::point(errLine > 0 ? errLine - 1 : 0, 0);
        d.message  = msg;
        d.severity = DiagnosticSeverity::Error;
        d.source   = "scx";
        result.diagnostics.push_back(std::move(d));
        return result;
    }

    // analyze() below re-preprocesses 'generated' from scratch, and
    // safec's real Preprocessor expands '#include' textually (splices in
    // the *entire* included file's own preprocessed content in place of
    // the directive line — see Preprocessor::handleInclude) — so every
    // line after an '#include' shifts by however many lines that
    // expansion contributed, and lineMap (built against transpileScx's
    // own un-flattened output) doesn't know about that shift at all.
    // Every markup-using .scx file has exactly this shape: two
    // auto-prepended '#include' lines, immediately followed by the user's
    // own content — so measure that one expansion's line count directly
    // (by preprocessing just those two lines with the same include search
    // path) and undo it before consulting lineMap. This is exact for the
    // common case (a .scx file with no '#include' of its own beyond the
    // auto-prepended pair); an additional user-written '#include'
    // elsewhere in the file would need its own correction this doesn't
    // account for, so positions after one may drift — no worse than the
    // same pre-existing limitation ordinary .sc files already have.
    bool hasHeader = generated.rfind("#include <std/scx/scx.h>\n", 0) == 0;
    int  includeOffset = 0; // (flattened line of first real content) - 3
    if (hasHeader) {
        // Preprocess 'generated' itself (the exact text analyze() below
        // will also preprocess) and find where its own unflattened line 3
        // — the first line of real content, right after the two
        // auto-prepended includes — actually lands once those includes
        // are expanded. Measuring against a separate, shorter probe
        // (e.g. just the two include lines followed by a synthetic marker)
        // was tried first and came up systematically one line short —
        // apparently the exact splice shape (what, if anything, follows
        // the includes) affects the included files' own trailing-newline
        // handling — so this measures the real text directly instead of
        // inferring from a stand-in.
        std::string firstContentLine;
        {
            size_t nl1 = generated.find('\n');
            size_t nl2 = (nl1 == std::string::npos) ? std::string::npos
                                                     : generated.find('\n', nl1 + 1);
            size_t nl3 = (nl2 == std::string::npos) ? std::string::npos
                                                     : generated.find('\n', nl2 + 1);
            if (nl2 != std::string::npos)
                firstContentLine = generated.substr(
                    nl2 + 1, nl3 == std::string::npos ? std::string::npos : nl3 - nl2 - 1);
        }

        // (Same settings as analyze() below, so includes expand alike.)
        const std::string &probeName = internFilename(uri);
        safec::DiagEngine probeDiag(probeName.c_str());
        probeDiag.setSilent(true);
        configureCHeaderImport(findSafec(), opts.target);
        safec::PreprocOptions probeOpts;
        probeOpts.importCHeaders = true;
        probeOpts.includePaths   = opts.includeDirs;
        probeOpts.compatMode     = opts.compat;
        probeOpts.freestanding   = opts.freestanding;
        for (const auto &d : opts.defines) probeOpts.cmdlineDefs[d.first] = d.second;
        safec::Preprocessor probe(generated, probeName, probeDiag, probeOpts);
        std::string flat = probe.process();

        auto anchorPos = firstContentLine.empty() ? std::string::npos
                                                    : flat.find(firstContentLine);
        if (anchorPos != std::string::npos) {
            int anchorFlatLine = 1;
            for (size_t k = 0; k < anchorPos; ++k) if (flat[k] == '\n') ++anchorFlatLine;
            includeOffset = anchorFlatLine - 3;
        }
    }

    AnalysisResult result = analyze(uri, generated, opts);

    // Undo the '#include' flattening shift, then look up lineMap[genLine]
    // (1-indexed) -> original .scx line (1-indexed; see ScxTranspiler.h).
    // Out-of-range clamps to the map's last entry rather than crashing.
    auto remapLine = [&](int line0) {
        int flatLine1 = line0 + 1;
        int genLine1  = flatLine1;
        if (hasHeader) {
            genLine1 = (flatLine1 <= includeOffset + 2) ? 1 : flatLine1 - includeOffset;
        }
        if (genLine1 >= 1 && genLine1 < static_cast<int>(lineMap.size()))
            return lineMap[genLine1] - 1;
        return (lineMap.size() > 1 ? lineMap.back() : genLine1) - 1;
    };
    for (auto &d : result.diagnostics) {
        d.range.start.line = remapLine(d.range.start.line);
        d.range.end.line   = remapLine(d.range.end.line);
        d.source = "scx";
    }
    for (auto &s : result.symbols) s.pos.line = remapLine(s.pos.line);

    return result;
}

// ── symbol lookup ───────────────────────────────────────────────────────────
// The identifier under the cursor, its range, and the 'X' of an 'X::word'
// spelling ("File::drop", "Vec::new", "std::alloc").
struct WordAt {
    std::string word, qualifier;
    int line = 0, start = 0, end = 0;
};

static WordAt wordUnder(const std::string &source, int line, int col) {
    WordAt w;
    size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        pos = source.find('\n', pos);
        if (pos == std::string::npos) return w;
        ++pos;
    }
    size_t eol = source.find('\n', pos);
    std::string text = source.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    if (col < 0 || col > (int)text.size()) return w;
    int s = col, e = col;
    while (s > 0 && isWord(text[s - 1])) --s;
    while (e < (int)text.size() && isWord(text[e])) ++e;
    if (s == e) return w;
    w.word = text.substr(s, e - s);
    w.line = line; w.start = s; w.end = e;
    int q = s;
    while (q > 0 && text[q - 1] == ' ') --q;
    if (q >= 2 && text[q - 1] == ':' && text[q - 2] == ':') {
        int qe = q - 2;
        while (qe > 0 && text[qe - 1] == ' ') --qe;
        int qs = qe;
        while (qs > 0 && isWord(text[qs - 1])) --qs;
        w.qualifier = text.substr(qs, qe - qs);
    }
    return w;
}

// Every symbol 'w' may name, best first: one of the qualifier's struct
// ('File::drop' -> File's drop, not Vec's), then one in this document, then
// a definition over a declaration.
static std::vector<const SymbolInfo *> candidates(const AnalysisResult &result, const WordAt &w,
                                                  const std::string &uri) {
    std::vector<const SymbolInfo *> out;
    bool ownerMatch = false;
    for (auto &sym : result.symbols) {
        if (sym.name != w.word) continue;
        out.push_back(&sym);
        if (!w.qualifier.empty() && sym.owner == w.qualifier) ownerMatch = true;
    }
    if (ownerMatch)
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const SymbolInfo *s) { return s->owner != w.qualifier; }),
                  out.end());
    auto score = [&](const SymbolInfo *s) {
        return (s->docUri == uri ? 4 : 0) + (s->hasBody ? 2 : 0) + (s->owner.empty() ? 1 : 0);
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const SymbolInfo *a, const SymbolInfo *b) { return score(a) > score(b); });
    return out;
}

// ── hover ─────────────────────────────────────────────────────────────────────
std::optional<Hover> hover(const AnalysisResult &result,
                           const std::string &source,
                           int line, int col) {
    WordAt w = wordUnder(source, line, col);
    if (w.word.empty()) return std::nullopt;
    auto cands = candidates(result, w, result.uri);
    if (cands.empty()) return std::nullopt;

    // One signature per distinct declaration (a method's declaration in its
    // struct and its definition print the same), at most a handful when an
    // unqualified method name could be several structs' (v.push(...)).
    std::vector<std::string> sigs;
    for (auto *c : cands) {
        if (std::find(sigs.begin(), sigs.end(), c->typeStr) == sigs.end()) sigs.push_back(c->typeStr);
        if (sigs.size() == 6) break;
    }
    Hover h;
    h.contents = "```safec\n";
    for (size_t i = 0; i < sigs.size(); ++i) h.contents += (i ? "\n" : "") + sigs[i];
    h.contents += "\n```";
    h.range = {{w.line, w.start}, {w.line, w.end}};
    return h;
}

// ── complete ──────────────────────────────────────────────────────────────────
std::vector<CompletionItem> complete(const AnalysisResult &result) {
    std::vector<CompletionItem> items;
    std::set<std::pair<std::string, std::string>> seen;   // (label, detail)

    // All top-level symbols (a function declared in a header and defined
    // here, or a method declared in its struct and defined out of line, once)
    for (auto &sym : result.symbols) {
        if (!seen.insert({sym.name, sym.typeStr}).second) continue;
        CompletionItem item;
        item.label  = sym.name;
        item.detail = sym.typeStr;
        switch (sym.kind) {
        case SymKind::Function:  item.kind = CompletionItemKind::Function; break;
        case SymKind::Method:    item.kind = CompletionItemKind::Method;   break;
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
        if (!seen.insert({*kw, ""}).second) continue;
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
    WordAt w = wordUnder(source, line, col);
    if (w.word.empty()) return std::nullopt;
    auto cands = candidates(result, w, uri);
    if (cands.empty()) return std::nullopt;
    const SymbolInfo *best = cands.front();
    // A C header's imported declarations have no file to jump to
    // ("<stdio.h>" is the importer's name for them, not a path).
    if (best->docUri.rfind("file://<", 0) == 0) return std::nullopt;
    Location loc;
    loc.uri   = best->docUri.empty() ? uri : best->docUri;
    loc.range = Range::point(best->pos.line, best->pos.character);
    return loc;
}

// ── documentSymbols ──────────────────────────────────────────────────────────
std::vector<DocumentSymbol> documentSymbols(const AnalysisResult &result) {
    auto mine = [&](const SymbolInfo &s) { return s.docUri.empty() || s.docUri == result.uri; };
    // Methods defined in this document: their in-struct declarations
    // (which Sema adds to the TU too) aren't listed again.
    std::set<std::string> defined;
    for (auto &s : result.symbols)
        if (mine(s) && s.kind == SymKind::Method && s.hasBody) defined.insert(s.owner + "::" + s.name);

    std::vector<DocumentSymbol> syms;
    std::set<std::string> listed;
    for (auto &s : result.symbols) {
        if (!mine(s)) continue;   // from a header
        std::string name = s.owner.empty() ? s.name : s.owner + "::" + s.name;
        if (s.kind == SymKind::Method && !s.hasBody && defined.count(name)) continue;
        if (s.kind == SymKind::Method && !listed.insert(name).second) continue;
        DocumentSymbol ds;
        ds.name  = name;
        ds.detail = s.typeStr;
        ds.range = Range::point(s.pos.line, s.pos.character);
        ds.selectionRange = ds.range;
        switch (s.kind) {
        case SymKind::Function:  ds.kind = SymbolKind::Function;  break;
        case SymKind::Method:    ds.kind = SymbolKind::Method;    break;
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
