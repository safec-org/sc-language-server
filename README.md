# sc-language-server

Language Server Protocol (LSP) server for SafeC (`.sc` files) and scx
(`.scx` — SafeC's JSX/TSX/RSX-style HTML templating language) files.

## Features

| Feature | Status |
|---------|--------|
| **Diagnostics** | ✅ Real-time errors/warnings on open/change |
| **Hover** | ✅ Type info for top-level symbols |
| **Completion** | ✅ All symbols + SafeC keywords |
| **Go-to-definition** | ✅ Jump to declaration |
| **Document Symbols** | ✅ Outline view of all declarations |

Because the server links the SafeC compiler's actual frontend sources
(`Preprocessor`/`Lexer`/`Parser`/`Sema`/`ConstEval` — see Architecture below),
every language construct the compiler understands is understood here too,
with no separate reimplementation to keep in sync: `namespace` blocks,
`generic<T>` monomorphization, the native `vec<T, N>` SIMD type, the full
C-superset surface (`__attribute__`, bitfields, designated initializers,
flexible array members, anonymous struct/union, compound literals,
`_Generic`, VLAs gated to `unsafe{}`), `constinit`/`consteval`/`const fn`,
and inline `asm{}` all get real diagnostics and hover info. Multi-target
codegen (`--target`, cross-compiling to other architectures/OSes) and the
`std::simd` per-ISA convenience headers are compiler/stdlib-level concerns
this server doesn't need to know about — it analyzes source, not machine
code, so it works identically regardless of which target a project
eventually builds for.

## Building

Requires: CMake 3.20+, C++17 compiler, the SafeC compiler source tree at
`../SafeC/compiler/` (or set `-DSAFEC_DIR=<path>`), and the safeguard
source tree at `../SafeC/safeguard/` (or set `-DSAFEGUARD_DIR=<path>`) —
the latter supplies `ScxTranspiler.cpp` and `Manifest.cpp`, referenced by
path rather than vendored so `safeguard` and this server share exactly one
copy of the `.scx` transpiler and the `Package.toml` parser.

```bash
cmake -S . -B build
cmake --build build
# Binary: build/sc-lsp
```

## Include Paths

The include path is the one `safeguard` passes `safec`:

- **The SafeC root** — the parent of `std/`, so `#include <std/...>`
  resolves and `<math.h>`, `<time.h>`, `<stdint.h>` & co. stay the C
  library's (std/ itself is never on the path). It is found as safeguard
  finds std/: `$SAFEC_HOME` when it holds a `std/`, else the installation
  `sc-lsp` runs from (`<prefix>/bin/sc-lsp` beside `<prefix>/std`, the
  release/`install.sh` layout), else the SafeC checkout the binary was
  built against (`<SAFEC_DIR>/..`, baked in at build time).
- **The project's `include/`** and its `[dependencies]`' header
  directories (a SafeC source package's `include/` and `src/`, a library's
  `include/`, or its `include = [...]`), when the file is inside a
  `Package.toml` project.
- A quoted `#include "x.h"` is found next to the file first.
- **C headers** (`<stdio.h>`, `<math.h>`, ...) are imported the way safec
  imports them: the server runs `safec --dump-pp` on `#include <hdr>` once
  per header (cached for the session) and uses the C declarations the
  compiler's clang-based importer produced. The `safec` used is `$SAFEC`,
  else `$SAFEC_HOME/bin/safec` or `$SAFEC_HOME/compiler/build/safec`, else
  the one beside `sc-lsp`, else the build-time checkout's, else `safec` on
  `PATH`.

`initializationOptions.includePaths` appends further directories:

```json
{ "initializationOptions": { "includePaths": ["/path/to/headers"] } }
```

## Analysis Settings

Each document is analyzed with the settings `safec` and `safeguard` would
use for it, so the server reports exactly what a build would:

- **Predefines** — the platform macro (`__linux__`/`__APPLE__`/`_WIN32`),
  `__SIZEOF_POINTER__`, `__SAFEC_FREESTANDING__` for bare-metal targets,
  and `__SAFEC_COMPAT_PREPROCESSOR__` in compat mode. Sema gets the same
  pointer width, so `size_t` and the C-ABI argument rules match the target.
- **Compat preprocessor** — on for files under the project's `tests/`
  directory and for the standard library (safeguard compiles both with
  `--compat-preprocessor`, e.g. for `ASSERT_*`), and for all of a project's
  files when its `Package.toml` sets `[build] compat_preprocessor = true`
  (e.g. for std/async's coroutine macros); off elsewhere.
- **Features** — the nearest `Package.toml` above the file is read and its
  enabled `[features]` (starting from `default`) become
  `SAFEC_FEATURE_<NAME>=1`, as `safeguard build` defines them.
- **Notes** — the compiler's notes on an error (`'b' declared here`,
  `'b' was dropped here` for ownership errors, the thread a `spawn_scoped`
  lends a value to) become the diagnostic's `relatedInformation`, plus a
  Hint at the noted place pointing back.
- **Headers** — diagnostics raised inside an `#include`d header are
  reported at the top of the document with the header's own file:line;
  go-to-definition on a library symbol opens the header; the outline lists
  only the document's own declarations. Hover shows full signatures with
  source-level names (`generic<T> int std::Vec::push(T value)`), and
  completion offers `push` rather than the mangled `Vec_push`.

Override any of it per workspace through `initializationOptions` (the VS
Code extension maps its `safec.*` settings onto these):

```json
{ "initializationOptions": {
    "includePaths": ["/path/to/headers"],
    "target": "arm-7em",
    "compatPreprocessor": true,
    "defines": { "BOARD_REV": "3" },
    "features": ["backend"],
    "noDefaultFeatures": false,
    "provides": ["malloc", "free"]
} }
```

`target` takes a bare-metal alias (`x86`, `x86_64`, `aarch32`, `aarch64`,
`thumb`, `thumb2`, `arm-6m`, `arm-7m`, `arm-7em`, `arm-8m`, `rv32i`,
`rv32e`, `rv64i`, `rv64e`) or an LLVM triple; bare-metal targets imply
freestanding mode. `Package.toml` parsing reuses safeguard's own
`Manifest.cpp`, referenced by path like the scx transpiler.

## File Types

Both `.sc` (implementation) and `.h` (declaration) files are analyzed —
the server itself is extension-agnostic (it just parses whatever text a
client sends it as a `safec` document); the SafeC standard library and
most real projects split declarations into `.h` and definitions into
`.sc`, both using SafeC's own grammar (a C superset), so both need the
same diagnostics/hover/completion. **Caveat:** `.h` is also the standard
C/C++ header extension — if you have a C/C++ extension installed in the
same editor, both will try to claim `.h` files. See each editor's section
below for how to resolve that; the general answer is that the *last*
registered/loaded association wins, so install order matters, and you can
always override the language mode per-file if a plain C header gets
mis-detected as SafeC (or vice versa).

### `.scx`

Unlike `.h`, `.scx` isn't just handed to `analyze()` as-is — raw scx
markup (`return <tag>...;`, see safec-docs' "scx Templating" page) isn't
valid SafeC syntax, so it's transpiled first (the same transpiler
`safeguard build` uses — see the Building section above), and every
diagnostic/hover/definition/document-symbol position that comes back is
mapped from the transpiled SafeC's line numbers back to the original
`.scx` buffer's own lines before being sent to the client — the client
never sees the generated code. This mapping is line-level, not
column-level, and is exact for content outside a markup expansion; a
diagnostic *inside* one (e.g. a type error on a `{expr}` interpolation)
points at the line the enclosing `return <markup>;` statement starts on,
not the exact sub-line the error occurred on. `didOpen`/`didChange` on a
`.scx` document handle all of this automatically — there's nothing
extension-specific for a client to configure beyond registering the file
extension (already done in `editors/vscode/`).

## Editor Integration

### VS Code

See `editors/vscode/` — sideload the extension:

```bash
cd editors/vscode
npm install
npm install -g @vscode/vsce
vsce package          # → safec-analyzer-<version>.vsix
code --install-extension safec-analyzer-*.vsix
```

Set `"safec.server.path": "/path/to/sc-lsp"` in VS Code settings.

The extension registers both `.sc` and `.h`. If Microsoft's C/C++
extension is also installed and you want its `.h` handling instead in a
particular file, use "Change Language Mode" (bottom-right status bar, or
`Cmd/Ctrl+K M`) to switch that file back to C/C++ — this is a per-file
override, not a global setting.

### Neovim (0.11+, built-in LSP)

```lua
vim.filetype.add({
  extension = { sc = "safec", scx = "safec" },
  -- .h is ambiguous with plain C/C++ headers: only claim it inside a
  -- SafeC project (Package.toml at or above the file).
  pattern = {
    [".*%.h"] = function(path)
      return vim.fs.find("Package.toml", { upward = true, path = vim.fs.dirname(path) })[1]
          and "safec" or nil
    end,
  },
})

vim.lsp.config("sc_lsp", {
  cmd = { "sc-lsp" },
  filetypes = { "safec" },
  root_markers = { "Package.toml", ".git" },
  -- init_options = { target = "arm-7em", features = { "backend" } },
})
vim.lsp.enable("sc_lsp")

-- Highlighting: C's syntax plus SafeC's own words.
vim.api.nvim_create_autocmd("FileType", {
  pattern = "safec",
  callback = function()
    vim.bo.syntax = "c"
    vim.cmd([[
      syn keyword safecKeyword region unsafe generic trait consteval constinit static_assert self operator new arena_reset arena_destroy arena_mark arena_free_to tuple namespace spawn join defer errdefer match packed try must_use fn fn_eval alignof typeof fieldcount naked interrupt section noreturn asm pure atomic newtype thread_local
      syn keyword safecRegion stack heap arena static contained
      syn match   safecRegionRef "?\=&\(stack\|heap\|arena\|static\)\>" contains=safecRegion
      syn keyword safecContextual consume impl needs_drop drop forget spawn_scoped malloc_defer some none is_null is_none Send Sync
      hi def link safecKeyword Keyword
      hi def link safecRegion StorageClass
      hi def link safecContextual Special
    ]])
  end,
})
```

With nvim-lspconfig on Neovim < 0.11, register the same server:

```lua
require("lspconfig.configs").sc_lsp = {
  default_config = {
    cmd = { "sc-lsp" },
    filetypes = { "safec" },
    root_dir = require("lspconfig.util").root_pattern("Package.toml", ".git"),
  },
}
require("lspconfig").sc_lsp.setup({})
```

### Emacs (Eglot)

```elisp
;; A mode of its own (derived from c-mode), so sc-lsp serves SafeC files
;; only and plain C buffers keep whatever server they had.
(define-derived-mode safec-mode c-mode "SafeC"
  "Major mode for SafeC (.sc, .scx)."
  (font-lock-add-keywords
   nil
   `((,(regexp-opt '("region" "unsafe" "generic" "trait" "consteval" "constinit" "static_assert" "self" "operator" "new" "arena_reset" "arena_destroy" "arena_mark" "arena_free_to" "tuple" "namespace" "spawn" "join" "defer" "errdefer" "match" "packed" "try" "must_use" "fn" "fn_eval" "alignof" "typeof" "fieldcount" "naked" "interrupt" "section" "noreturn" "asm" "pure" "atomic" "newtype" "thread_local") 'symbols)
      . font-lock-keyword-face)
     (,(regexp-opt '("consume" "impl" "needs_drop" "drop" "forget" "spawn_scoped" "malloc_defer" "some" "none" "is_null" "is_none" "Send" "Sync") 'symbols)
      . font-lock-builtin-face)
     ("\\??&\\(stack\\|heap\\|arena\\|static\\)\\_>" 1 font-lock-type-face))))

(add-to-list 'auto-mode-alist '("\\.scx?\\'" . safec-mode))
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(safec-mode . ("sc-lsp"))))
(add-hook 'safec-mode-hook #'eglot-ensure)

;; .h is ambiguous with C headers: switch a SafeC header with
;; M-x safec-mode, or give it a first line of  // -*- mode: safec -*-
```

## Architecture

```
stdin ──► Transport (Content-Length framing)
             │
             ▼
          Server (JSON-RPC dispatcher)
             │
             ▼
          Analysis (SafeC pipeline wrapper)
             │
      Preprocessor → Lexer → Parser → Sema → ConstEval
```

The SafeC frontend is compiled as `libsafec_frontend` with **zero LLVM
dependencies** — the server binary is small and starts instantly.

## Options

```
sc-lsp [--log-file <path>]
```

- `--log-file <path>` — write debug log to file (default: discarded)

## Smoke Test

```bash
printf 'Content-Length: 119\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":null,"capabilities":{}}}' \
  | ./build/sc-lsp | head -3
```
