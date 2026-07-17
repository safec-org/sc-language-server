# SafeC Analyzer

Language support for [SafeC](https://github.com/safec-org/SafeC) — a
deterministic, region-aware, compile-time-first systems programming
language that preserves C ABI compatibility while enforcing memory safety,
type safety, and real-time determinism at compile time.

This extension talks to `sc-lsp`, a language server built directly on top
of the SafeC compiler's own frontend (`Preprocessor → Lexer → Parser →
Sema → ConstEval`), so it understands exactly what the compiler understands
— no separate, drifting reimplementation of the grammar.

## Features

- **Syntax highlighting** — regions (`&stack`/`&heap`/`&arena<R>`/`&static`),
  generics, `namespace`, the native `vec<T, N>` SIMD type, pattern matching,
  bare-metal keywords (`naked`, `interrupt`, `section`), and the full
  C-superset surface (bitfields, designated initializers, `_Generic`,
  compound literals, `__attribute__`).
- **Real-time diagnostics** — errors and warnings from the actual compiler
  pipeline as you type, not a separate linter's approximation.
- **Hover** — type information for top-level symbols.
- **Completion** — every in-scope symbol, plus SafeC keywords and built-ins
  (atomics, volatile MMIO access, bit-manipulation intrinsics, ARM Cortex-M
  DSP-extension intrinsics).
- **Go to Definition** — jump straight from a use to its declaration.
- **Document Symbols** — outline view of every top-level declaration.

## File types

Both `.sc` (implementation) and `.h` (declaration) files are supported —
SafeC projects and the standard library both split declarations into `.h`
and definitions into `.sc`, and both use SafeC's grammar. `.h` is also the
standard C/C++ header extension: if you have a C/C++ extension installed
too, both extensions will claim `.h`, and whichever was registered last
wins by default. Use **Change Language Mode**
(bottom-right status bar, or `Cmd/Ctrl+K M`) to override the language for
an individual file at any time — it's a per-file setting, not global.

## Requirements

This extension is a thin client — it needs the `sc-lsp` server binary
installed separately:

```bash
git clone https://github.com/safec-org/SafeC.git && cd SafeC
bash install.sh   # builds the compiler, stdlib, safeguard, and sc-lsp
```

`install.sh` places `sc-lsp` on your `PATH` by default. If it isn't found
automatically, point the extension at it explicitly (see Settings below).

## Settings

| Setting | Default | Description |
|---|---|---|
| `safec.server.path` | `sc-lsp` | Path to the `sc-lsp` binary — an absolute path, or a name resolvable on `PATH`. |
| `safec.server.args` | `[]` | Extra arguments passed to `sc-lsp` on launch. |
| `safec.trace.server` | `off` | LSP wire-protocol tracing (`off` / `messages` / `verbose`) for debugging the client-server connection. |

## More

Full build-from-source instructions, editor setups beyond VS Code
(Neovim, Emacs), and the server's internal architecture are documented in
the
[sc-language-server repository](https://github.com/safec-org/sc-language-server).
The SafeC language itself is documented at the
[official docs site](https://safec-org.github.io/safec-docs/).

## License

MIT
