#pragma once
#include <string>

namespace lsp {

// The LSP's CHeaderImporter (src/CHeaderImporterStub.cpp) links no LLVM/
// clang of its own: it asks the safec binary for what its real importer
// makes of a C header ('safec --dump-pp' of '#include <hdr>'), once per
// header, include path and target for the server's lifetime. Since
// safeguard passes only std's parent directory as -I, <math.h>, <time.h>,
// <stdio.h> & co. are libc's headers, which safec imports through clang;
// without this the preprocessor fell back to pasting glibc's raw headers,
// which SafeC's parser can't read.
//
// Set before each analysis: the safec to run ("" disables the import, and
// the preprocessor falls back to the raw header) and the --target ("" =
// host).
void configureCHeaderImport(const std::string &safecPath, const std::string &target);

} // namespace lsp
