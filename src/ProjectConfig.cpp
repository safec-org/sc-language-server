#include "ProjectConfig.h"
#include "Manifest.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

// The directory sc-lsp's own executable is in ("" if unknown).
fs::path selfDir() {
    std::error_code ec;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    fs::path p = fs::canonical(buf, ec);
    return ec ? fs::path(buf).parent_path() : p.parent_path();
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
#endif
}

const char *env(const char *name) {
    const char *v = std::getenv(name);
    return v && *v ? v : nullptr;
}

bool isDir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool isFile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

#ifdef _WIN32
const char *kExe = ".exe";
#else
const char *kExe = "";
#endif

// The header directories safeguard puts on a project's -I path for its
// [dependencies] (Builder::depLayout): 'include = [...]' when given, else
// a SafeC source package's include/ and src/, or a library's include/.
void dependencyIncludes(const safeguard::Manifest &m, const fs::path &root,
                        std::vector<std::string> &out) {
    for (const auto &dep : m.dependencies) {
        fs::path dir;
        if (!dep.path.empty()) {
            fs::path p(dep.path);
            dir = (p.is_absolute() ? p : root / p).lexically_normal();
        } else if (!dep.version.empty()) {
            dir = root / "deps" / dep.name;
        }
        fs::path base = dir.empty() ? root : dir;
        if (!dep.includes.empty()) {
            for (const auto &i : dep.includes) {
                fs::path p(i);
                p = (p.is_absolute() ? p : base / p).lexically_normal();
                if (isDir(p)) out.push_back(p.string());
            }
            continue;
        }
        if (dir.empty() || !isDir(dir)) continue;
        if (dep.build.empty() && isDir(dir / "src")) {
            if (isDir(dir / "include")) out.push_back((dir / "include").string());
            out.push_back((dir / "src").string());
        } else if (isDir(dir / "include")) {
            out.push_back((dir / "include").string());
        } else if (!dep.build.empty()) {
            out.push_back(dir.string());
        }
    }
}

std::string featureMacro(const std::string &name) {
    std::string upper = "SAFEC_FEATURE_";
    for (char c : name) upper += (char)std::toupper((unsigned char)(c == '-' ? '_' : c));
    return upper;
}

// [build] compat_preprocessor, on a SafeC whose safeguard Manifest has the
// field (older ones don't; then it's off, as safeguard itself had no key).
template <class M>
auto manifestCompatPreprocessor(const M &m, int) -> decltype(bool(m.build.compatPreprocessor)) {
    return m.build.compatPreprocessor;
}
template <class M>
bool manifestCompatPreprocessor(const M &, long) { return false; }

} // namespace

std::string safecRoot() {
    if (const char *home = env("SAFEC_HOME"))
        if (isDir(fs::path(home) / "std")) return fs::path(home).lexically_normal().string();
    fs::path self = selfDir();
    if (!self.empty() && isDir(self.parent_path() / "std"))
        return self.parent_path().string();
#ifdef SAFEC_STD_DIR
    return SAFEC_STD_DIR;
#else
    return {};
#endif
}

std::string findSafec() {
    if (const char *e = env("SAFEC")) return e;
    // The rest only changes with the environment: look it up once.
    static std::string found;
    if (!found.empty()) return found;
    std::string exe = std::string("safec") + kExe;
    std::vector<fs::path> candidates;
    if (const char *home = env("SAFEC_HOME")) {
        candidates.push_back(fs::path(home) / "bin" / exe);
        candidates.push_back(fs::path(home) / "compiler" / "build" / exe);
    }
    fs::path self = selfDir();
    if (!self.empty()) candidates.push_back(self / exe);
#ifdef SAFEC_STD_DIR
    candidates.push_back(fs::path(SAFEC_STD_DIR) / "compiler" / "build" / exe);
#endif
    found = "safec";
    for (const auto &c : candidates)
        if (isFile(c)) { found = c.string(); break; }
    return found;
}

std::string uriToPath(const std::string &uri) {
    std::string s = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string out;
    auto hex = [](char c) { return std::isxdigit((unsigned char)c) != 0; };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) && hex(s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);   // /C:/x -> C:/x
    return out;
}

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
    o.provides    = settings.provides;
    o.target      = settings.target;
    std::string sroot = safecRoot();
    if (!sroot.empty()) o.includeDirs.push_back(sroot);

    TargetInfo t = targetInfo(settings.target);
    o.pointerBits  = t.pointerBits;
    o.freestanding = t.bareMetal;
    if (!t.platformMacro.empty()) o.defines[t.platformMacro] = "1";
    o.defines["__SIZEOF_POINTER__"] = std::to_string(t.pointerBits / 8);
    if (o.freestanding) o.defines["__SAFEC_FREESTANDING__"] = "1";

    fs::path file = fs::path(path).lexically_normal();
    fs::path root = findProjectRoot(file.parent_path());

    bool manifestCompat = false;
    if (!root.empty()) {
        if (isDir(root / "include")) o.includeDirs.push_back((root / "include").string());
        try {
            safeguard::Manifest m = safeguard::ManifestParser::parseFile((root / "Package.toml").string());
            for (const auto &f : safeguard::resolveFeatures(m, settings.features, settings.noDefaultFeatures))
                o.defines[featureMacro(f)] = "1";
            manifestCompat = manifestCompatPreprocessor(m, 0);
            dependencyIncludes(m, root, o.includeDirs);
        } catch (...) {
            // A malformed manifest is safeguard's error to report; analyze
            // the file without feature defines rather than not at all.
        }
    }

    o.includeDirs.insert(o.includeDirs.end(), settings.includeDirs.begin(), settings.includeDirs.end());

    // safeguard compiles tests/ and the standard library with
    // --compat-preprocessor (they use ASSERT_* and similar macros), and the
    // project's own sources with it only when Package.toml sets
    // [build] compat_preprocessor = true (e.g. for std/async's coroutine
    // macros).
    if (settings.compat >= 0) {
        o.compat = settings.compat != 0;
    } else {
        bool inStd = false;
        for (const auto &inc : o.includeDirs)
            if (isUnder(file, fs::path(inc) / "std")) inStd = true;
        o.compat = inStd || isUnder(file, root / "tests") || manifestCompat;
    }
    if (o.compat) o.defines["__SAFEC_COMPAT_PREPROCESSOR__"] = "1";

    for (const auto &d : settings.defines) o.defines[d.first] = d.second;
    return o;
}

} // namespace lsp
