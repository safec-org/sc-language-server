// CHeaderImporter for the LSP server build (see CHeaderBridge.h).
//
// The real implementation (SafeC's compiler/src/CHeaderImporter.cpp) runs
// clang's AST dump and parses it with LLVM's JSON library; the LSP links no
// LLVM, so this one runs the safec binary instead and takes the imported
// declarations out of its preprocessed output — exactly the text the
// compiler itself pastes in for the header, so the LSP sees the same C
// declarations 'safec'/'safeguard build' do. Results are cached for the
// server's lifetime: each header costs one safec run (~0.1-0.3 s), the
// first time a document includes it.
//
// Compiled into safec_frontend (-fno-exceptions): no throwing calls here.
#include "safec/CHeaderImporter.h"
#include "CHeaderBridge.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define SC_POPEN  _popen
#define SC_PCLOSE _pclose
#define SC_GETPID _getpid
#else
#include <unistd.h>
#define SC_POPEN  popen
#define SC_PCLOSE pclose
#define SC_GETPID getpid
#endif

namespace fs = std::filesystem;

namespace {

std::string g_safec;
std::string g_target;
std::map<std::string, std::string> g_cache;   // key: target|paths|header -> declarations

const char kBegin[] = "// ---- begin C header import ----";
const char kEnd[]   = "// ---- end C header import ----";

std::string quoteArg(const std::string &s) {
#ifdef _WIN32
    std::string q = "\"";
    for (char c : s) { if (c == '"') q += '\\'; q += c; }
    return q + "\"";
#else
    std::string q = "'";
    for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
    return q + "'";
#endif
}

// What safec makes of '#include <header>': the text between its import
// markers, or "" when it didn't import the header (no clang, a header clang
// can't find, ...).
std::string runSafecImport(const std::string &header, const std::vector<std::string> &includePaths) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) return "";
    static unsigned counter = 0;
    fs::path src = dir / ("sc-lsp-" + std::to_string((long long)SC_GETPID()) + "-" +
                          std::to_string(counter++) + ".sc");
    {
        std::ofstream out(src);
        if (!out) return "";
        out << "#include <" << header << ">\n";
    }
    std::string cmd = quoteArg(g_safec) + " --dump-pp";
    for (const auto &p : includePaths) cmd += " -I " + quoteArg(p);
    if (!g_target.empty()) cmd += " " + quoteArg("--target=" + g_target);
    cmd += " " + quoteArg(src.string());
#ifdef _WIN32
    cmd = "\"" + cmd + " 2>NUL\"";   // cmd.exe strips one pair of outer quotes
#else
    cmd += " 2>/dev/null";
#endif
    std::string text;
    if (FILE *p = SC_POPEN(cmd.c_str(), "r")) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
        SC_PCLOSE(p);
    }
    fs::remove(src, ec);

    size_t b = text.find(kBegin);
    size_t e = text.rfind(kEnd);
    if (b == std::string::npos || e == std::string::npos || e < b) return "";
    return text.substr(b, e + sizeof(kEnd) - 1 - b) + "\n";
}

} // namespace

namespace lsp {
void configureCHeaderImport(const std::string &safecPath, const std::string &target) {
    g_safec  = safecPath;
    g_target = target;
}
} // namespace lsp

namespace safec {

CHeaderImporter::CHeaderImporter(DiagEngine &diag) : diag_(diag) {
    clangPath_ = g_safec;   // available() <=> a safec to ask
}

std::string CHeaderImporter::import(const std::string &headerName,
                                    const std::vector<std::string> &includePaths,
                                    SourceLocation /*loc*/) {
    // As the real importer does: a header's declarations go in once per
    // translation unit, and a repeat yields "" (on which the preprocessor
    // currently falls back to the raw system header, as safec does too).
    if (!imported_.insert(headerName).second) return "";
    std::string key = g_target + "|";
    for (const auto &p : includePaths) key += p + "|";
    key += headerName;
    auto it = g_cache.find(key);
    if (it == g_cache.end()) it = g_cache.emplace(key, runSafecImport(headerName, includePaths)).first;
    return it->second;
}

std::string CHeaderImporter::findClang() { return ""; }

std::string CHeaderImporter::runClangASTDump(const std::string &,
                                              const std::vector<std::string> &) {
    return "";
}

std::string CHeaderImporter::buildDeclarations(const std::string &) { return ""; }

std::string CHeaderImporter::cleanType(const std::string &qt) { return qt; }

bool CHeaderImporter::hasFunctionPointer(const std::string &) { return false; }

} // namespace safec
