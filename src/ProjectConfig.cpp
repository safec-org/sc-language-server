#include "ProjectConfig.h"
#include "Manifest.h"

#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace lsp {

namespace {

// The safec driver's bare-metal aliases (compiler/src/main.cpp,
// bareMetalTargets()); every one is a no-OS target.
struct Alias { const char *name; unsigned bits; };
const Alias kAliases[] = {
    {"x86", 32},    {"x86_64", 64}, {"aarch32", 32}, {"aarch64", 64},
    {"thumb", 32},  {"thumb2", 32}, {"arm-6m", 32},  {"arm-7m", 32},
    {"arm-7em", 32}, {"arm-8m", 32}, {"rv32i", 32},  {"rv32e", 32},
    {"rv64i", 64},  {"rv64e", 64},
};

bool startsWith(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string &s, const char *p) { return s.find(p) != std::string::npos; }

std::string hostPlatformMacro() {
#if defined(__APPLE__)
    return "__APPLE__";
#elif defined(_WIN32)
    return "_WIN32";
#elif defined(__linux__)
    return "__linux__";
#else
    return "";
#endif
}

// Nearest directory at or above `dir` that holds a Package.toml.
fs::path findProjectRoot(fs::path dir) {
    std::error_code ec;
    while (!dir.empty()) {
        if (fs::exists(dir / "Package.toml", ec)) return dir;
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool isUnder(const fs::path &file, const fs::path &dir) {
    if (dir.empty()) return false;
    auto f = file.lexically_normal().string();
    auto d = dir.lexically_normal().string();
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return f.rfind(d, 0) == 0;
}

std::string featureMacro(const std::string &name) {
    std::string upper = "SAFEC_FEATURE_";
    for (char c : name) upper += (char)std::toupper((unsigned char)(c == '-' ? '_' : c));
    return upper;
}

} // namespace

TargetInfo targetInfo(const std::string &target) {
    TargetInfo t;
    if (target.empty()) {
        t.pointerBits = sizeof(void *) * 8;
        t.platformMacro = hostPlatformMacro();
        return t;
    }
    for (const auto &a : kAliases) {
        if (target == a.name) { t.pointerBits = a.bits; t.bareMetal = true; return t; }
    }
    // An LLVM triple: arch-vendor-os[-env].
    std::string arch = target.substr(0, target.find('-'));
    if (startsWith(arch, "i") && arch.size() == 4 && arch.substr(2) == "86") t.pointerBits = 32;
    else if (startsWith(arch, "arm") || startsWith(arch, "thumb") || arch == "riscv32" ||
             arch == "wasm32" || arch == "mips" || arch == "mipsel" || arch == "powerpc")
        t.pointerBits = 32;
    else if (arch == "avr" || arch == "msp430") t.pointerBits = 16;
    if (contains(target, "apple") || contains(target, "darwin")) t.platformMacro = "__APPLE__";
    else if (contains(target, "windows") || contains(target, "win32")) t.platformMacro = "_WIN32";
    else if (contains(target, "linux")) t.platformMacro = "__linux__";
    t.bareMetal = contains(target, "-none") || (contains(target, "-elf") && !contains(target, "linux"));
    return t;
}

AnalysisOptions optionsFor(const std::string &path, const ServerSettings &settings) {
    AnalysisOptions o;
    o.includeDirs = settings.includeDirs;
    o.provides    = settings.provides;

    TargetInfo t = targetInfo(settings.target);
    o.pointerBits  = t.pointerBits;
    o.freestanding = t.bareMetal;
    if (!t.platformMacro.empty()) o.defines[t.platformMacro] = "1";
    o.defines["__SIZEOF_POINTER__"] = std::to_string(t.pointerBits / 8);
    if (o.freestanding) o.defines["__SAFEC_FREESTANDING__"] = "1";

    fs::path file = fs::path(path).lexically_normal();
    fs::path root = findProjectRoot(file.parent_path());

    // safeguard compiles tests/ and the standard library with
    // --compat-preprocessor (they use ASSERT_* and similar macros), and the
    // project's own sources without it.
    if (settings.compat >= 0) {
        o.compat = settings.compat != 0;
    } else {
        bool inStd = false;
        for (const auto &inc : settings.includeDirs)
            if (isUnder(file, fs::path(inc) / "std")) inStd = true;
        o.compat = inStd || isUnder(file, root / "tests");
    }
    if (o.compat) o.defines["__SAFEC_COMPAT_PREPROCESSOR__"] = "1";

    if (!root.empty()) {
        try {
            safeguard::Manifest m = safeguard::ManifestParser::parseFile((root / "Package.toml").string());
            for (const auto &f : safeguard::resolveFeatures(m, settings.features, settings.noDefaultFeatures))
                o.defines[featureMacro(f)] = "1";
        } catch (...) {
            // A malformed manifest is safeguard's error to report; analyze
            // the file without feature defines rather than not at all.
        }
    }

    for (const auto &d : settings.defines) o.defines[d.first] = d.second;
    return o;
}

} // namespace lsp
