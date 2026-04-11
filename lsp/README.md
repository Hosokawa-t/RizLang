# Riz LSP (minimal)

Uses the host **`riz check <file>`** command, which prints **one NDJSON object per parse error** to stdout. This server forwards them as LSP diagnostics.

## Build

```bash
cd lsp
npm install
npm run build
```

## Run (stdio)

From the repo root, with `riz` / `riz.exe` on `PATH`, or set **`RIZ_PATH`** to the full path of the interpreter:

```bash
export RIZ_PATH="$PWD/riz"   # Linux/macOS
node lsp/out/server.js --stdio
```

Windows (PowerShell):

```powershell
$env:RIZ_PATH = "$PWD\riz.exe"
node lsp/out/server.js --stdio
```

## Editor settings (VS Code / Cursor)

Point your editor’s **generic Language Server** config at the command above, or use workspace settings if your client supports a `settings.json` hook for custom servers.

Optional setting (if your client forwards it to the server):

- `riz.executablePath` — full path to `riz` / `riz.exe` (same as `RIZ_PATH`).

Register `*.riz` as a language if you want file-type detection; until a grammar exists, **Plain Text** + diagnostics still works.
