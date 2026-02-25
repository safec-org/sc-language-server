# sc-language-server

Language Server Protocol (LSP) server for SafeC (`.sc` files).

## Features

| Feature | Status |
|---------|--------|
| **Diagnostics** | ✅ Real-time errors/warnings on open/change |
| **Hover** | ✅ Type info for top-level symbols |
| **Completion** | ✅ All symbols + SafeC keywords |
| **Go-to-definition** | ✅ Jump to declaration |
| **Document Symbols** | ✅ Outline view of all declarations |

## Building

Requires: CMake 3.20+, C++17 compiler, and the SafeC compiler source tree
at `../SafeC/compiler/` (or set `-DSAFEC_DIR=<path>`).

```bash
cmake -S . -B build
cmake --build build
# Binary: build/sc-lsp
```

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

-- Associate .sc with safec filetype
vim.filetype.add({ extension = { sc = "safec" } })
```

### Emacs (Eglot)

```elisp
(add-to-list 'auto-mode-alist '("\\.sc\\'" . c-mode))
(add-to-list 'eglot-server-programs '(c-mode . ("sc-lsp")))
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
