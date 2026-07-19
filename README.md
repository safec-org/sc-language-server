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
the latter supplies `ScxTranspiler.cpp`, referenced by path rather than
vendored so `safeguard build` and this server share exactly one copy of
the `.scx` transpiler.

```bash
cmake -S . -B build
cmake --build build
# Binary: build/sc-lsp
```

## Standard Library Resolution

`#include <std/...>` is resolved against a default include path baked into
the binary at build time: `<SAFEC_DIR>/..` (the SafeC repo root — the
parent of both `compiler/` and `std/`), derived from the same `SAFEC_DIR`
used to locate the frontend sources above. This means stdlib headers
(hover, completion, diagnostics — including nullable/optional match-forcing
errors, which are just `Sema` diagnostics like any other) resolve out of
the box for a project sitting next to the SafeC checkout this binary was
built against.

If your project uses a different SafeC checkout (or the LSP binary was
built elsewhere and copied), extend the search path per-client via
`initialize`'s `initializationOptions`:

```json
{ "initializationOptions": { "includePaths": ["/path/to/other/SafeC"] } }
```

This is appended to (not a replacement for) the compiled-in default.

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
vsce package          # → safec-1.0.0.vsix
code --install-extension safec-1.0.0.vsix
```

Set `"safec.server.path": "/path/to/sc-lsp"` in VS Code settings.

The extension registers both `.sc` and `.h`. If Microsoft's C/C++
extension is also installed and you want its `.h` handling instead in a
particular file, use "Change Language Mode" (bottom-right status bar, or
`Cmd/Ctrl+K M`) to switch that file back to C/C++ — this is a per-file
override, not a global setting.

### Neovim (nvim-lspconfig)

```lua
require("lspconfig.configs").safec = {
  default_config = {
    cmd = { "sc-lsp" },
    filetypes = { "safec" },
    root_dir = require("lspconfig.util").find_git_ancestor,
  },
}
require("lspconfig").safec.setup({})

-- Associate .sc with safec filetype unconditionally.
vim.filetype.add({ extension = { sc = "safec" } })

-- .h is ambiguous with plain C/C++ headers — only claim it inside a
-- SafeC project (Package.toml at or above the file), so C/C++ headers
-- elsewhere on your system are unaffected.
vim.filetype.add({
  pattern = {
    [".*%.h"] = function(path)
      return vim.fs.find("Package.toml", { upward = true, path = vim.fs.dirname(path) })[1]
          and "safec" or nil
    end,
  },
})
```

### Emacs (Eglot)

```elisp
(add-to-list 'auto-mode-alist '("\\.sc\\'" . c-mode))
(add-to-list 'eglot-server-programs '(c-mode . ("sc-lsp")))

;; .h is ambiguous with plain C/C++ headers; c-mode is a reasonable
;; default there already (SafeC is a C superset), so eglot-server-programs
;; above already covers .h once you're in c-mode — no separate
;; auto-mode-alist entry needed unless you use a different major mode
;; for C headers than for .sc files.
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
