#pragma once
#include <map>
#include <string>
#include <vector>

namespace lsp {

// How a document is analyzed — the same knobs the safec driver derives from
// its command line, so the LSP reports exactly what `safec`/`safeguard`
// would for that file.
struct AnalysisOptions {
    std::vector<std::string>           includeDirs;
    std::map<std::string, std::string> defines;      // -D NAME=VALUE (incl. predefines)
    bool                               compat       = false;  // --compat-preprocessor
    bool                               freestanding = false;  // --freestanding (or a bare-metal --target)
    unsigned                           pointerBits  = 64;     // target pointer width
    std::vector<std::string>           provides;     // --provides
};

// Workspace-wide settings from initialize's initializationOptions. Every
// field is optional; unset fields fall back to what safeguard would do.
//   includePaths:        extra -I directories
//   target:              bare-metal alias (see `safec --list-targets`) or LLVM triple
//   compatPreprocessor:  true/false to force; unset = safeguard's rule
//                        (tests/ and std/ sources: on, everything else: off)
//   defines:             { "NAME": "VALUE", ... }
//   features:            extra Package.toml features to enable
//   noDefaultFeatures:   don't start from the manifest's "default" feature
//   provides:            function names another translation unit defines
struct ServerSettings {
    std::vector<std::string>           includeDirs;
    std::string                        target;
    int                                compat = -1;   // -1 = auto
    std::map<std::string, std::string> defines;
    std::vector<std::string>           features;
    bool                               noDefaultFeatures = false;
    std::vector<std::string>           provides;
};

// Options for the file at `path` (a filesystem path, not a URI): applies
// the settings, the target's predefines, and the enclosing project's
// Package.toml [features] as SAFEC_FEATURE_<NAME>=1 defines.
AnalysisOptions optionsFor(const std::string &path, const ServerSettings &settings);

// Pointer width and bare-metal-ness of a target alias or triple ("" = host).
struct TargetInfo {
    unsigned    pointerBits = 64;
    bool        bareMetal   = false;
    std::string platformMacro;   // "__linux__", "__APPLE__", "_WIN32" or ""
};
TargetInfo targetInfo(const std::string &target);

} // namespace lsp
