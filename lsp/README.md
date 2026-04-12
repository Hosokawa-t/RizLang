# Riz Language Server (LSP)

Node.js implementation of the [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) for **Riz** `.riz` sources. It shells out to the native **`riz check`** binary for accurate parse diagnostics (NDJSON, including **line + column ranges** when built with Riz ≥ 0.9.3), and adds editor features that do not require duplicating the parser in TypeScript.

## Features

| Capability | Description |
|------------|-------------|
| **Diagnostics** | Debounced `riz check` on open / change / save; maps `startColumn` / `endColumn` when present. |
| **Completion** | Keywords, builtins (with docs), prefix filtering, snippets (`fn`, `let`, `for`, `if`, `struct`). |
| **Hover** | Builtins, keywords, and same-file definitions (`fn` / `let` / `struct` / …). |
| **Go to definition** | Same-file jump to `fn` / `let` / `mut` / `struct` / `trait` / `impl` / `for` binding. |
| **Document symbols** | Outline of functions, variables, structs, traits, impls. |
| **Workspace symbol** | Query symbols in **open** `.riz` buffers (up to 200 hits). |
| **Signature help** | Selected builtins after `(` with active parameter from comma count. |

## Requirements

- **Node.js** ≥ 18  
- **`riz` / `riz.exe` on `PATH`**, or set **`RIZ_PATH`** / VS Code **`riz.executablePath`** to the interpreter (must support `riz check` with column fields for best underline accuracy).

## Build

```bash
cd lsp
npm install
npm run build
```

Output: `lsp/out/server.js`.

## Run (stdio)

From the repo root:

```bash
export RIZ_PATH="$PWD/riz"   # Linux/macOS
node lsp/out/server.js --stdio
```

Windows (PowerShell):

```powershell
$env:RIZ_PATH = "$PWD\riz.exe"
node lsp/out/server.js --stdio
```

## Configuration (client → server)

| Key | Effect |
|-----|--------|
| `riz.executablePath` | Full path to `riz` / `riz.exe`. |
| `riz.diagnosticDelay` | Debounce in ms after edits before `riz check` (default `200`). |

## VS Code / Cursor extension

Use the **`editors/riz-vscode`** workspace: syntax highlighting, grammar, and automatic LSP startup for `*.riz`. The VSIX prepublish step bundles `lsp/out/server.js` under `bundled/lsp/`.

## NDJSON shape (`riz check`)

Each diagnostic is one JSON object per line, e.g.:

```json
{"line":2,"startColumn":2,"endColumn":5,"message":"…","source":"riz-parse","file":"path.riz"}
```

`line` is **1-based**; `startColumn` / `endColumn` are **0-based UTF-8 byte offsets** on that line (`endColumn` is exclusive). Older Riz builds may omit the column fields; the LSP still falls back to full-line ranges.
