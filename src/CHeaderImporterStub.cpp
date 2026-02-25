// Stub implementation of CHeaderImporter for the LSP server build.
// The real implementation uses LLVM JSON to parse clang's AST dump,
// which we don't need for the LSP (importCHeaders is always false in LSP mode).
#include "safec/CHeaderImporter.h"

namespace safec {

CHeaderImporter::CHeaderImporter(DiagEngine &diag) : diag_(diag) {
    // stub: no clang path — available() returns false
}

std::string CHeaderImporter::import(const std::string & /*headerName*/,
                                    const std::vector<std::string> & /*includePaths*/) {
    return "";
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
