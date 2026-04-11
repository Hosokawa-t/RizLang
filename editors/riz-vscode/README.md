# Riz for VS Code / Cursor

Syntax highlighting for `.riz` and integration with the **Riz LSP** (`lsp/out/server.js`), which runs `riz check` for parse diagnostics.

## Prereqs

1. Build the interpreter (`riz` / `riz.exe`) — repo root `build.bat` or `make`.
2. Build the language server: `cd lsp && npm install && npm run build`.

## Develop this extension

1. Open **`editors/riz-vscode`** as the workspace folder in VS Code or Cursor (or multi-root with the repo root so `lsp/out/server.js` exists).
2. Run **Run → Start Debugging** (`Run Riz Extension`).
3. In the Extension Development Host, open a folder that contains **`lsp/out/server.js`** (e.g. the full Riz repo) and open a `.riz` file.

## Settings

| Key | Meaning |
|-----|--------|
| `riz.executablePath` | Path to `riz` / `riz.exe` for `riz check`. |
| `riz.lspServerScript` | Optional absolute path to `server.js` if not under `<workspace>/lsp/out/server.js`. |
| `riz.nodePath` | Optional Node binary (default: current Node). |

## Package VSIX

```bash
cd editors/riz-vscode
npm install
npm run compile
npx vsce package
```

Install the generated `.vsix` via **Extensions → … → Install from VSIX…**.

For a self-contained VSIX you would bundle `lsp/` (see repo `lsp/README.md`); this extension expects a workspace checkout or manual `riz.lspServerScript`.
