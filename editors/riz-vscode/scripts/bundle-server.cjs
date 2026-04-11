/**
 * Copy built Riz LSP (lsp/out/server.js + production deps) into bundled/lsp/
 * for self-contained VSIX packaging. Run from repo after: cd lsp && npm ci && npm run build
 */
const fs = require("fs");
const path = require("path");
const { execSync } = require("child_process");

const extRoot = path.join(__dirname, "..");
/* extRoot = …/editors/riz-vscode → repo root is parent of editors/ */
const repoRoot = path.join(extRoot, "..", "..");
const lspSrc = path.join(repoRoot, "lsp");
const dest = path.join(extRoot, "bundled", "lsp");
const serverSrc = path.join(lspSrc, "out", "server.js");

if (!fs.existsSync(serverSrc)) {
  console.error(
    "[bundle-server] Missing " +
      serverSrc +
      "\n  Build the language server first:  cd lsp && npm ci && npm run build"
  );
  process.exit(1);
}

if (fs.existsSync(dest)) fs.rmSync(dest, { recursive: true, force: true });
fs.mkdirSync(path.join(dest, "out"), { recursive: true });

fs.copyFileSync(serverSrc, path.join(dest, "out", "server.js"));
fs.copyFileSync(path.join(lspSrc, "package.json"), path.join(dest, "package.json"));
const lock = path.join(lspSrc, "package-lock.json");
if (fs.existsSync(lock)) {
  fs.copyFileSync(lock, path.join(dest, "package-lock.json"));
}

console.log("[bundle-server] npm ci --omit=dev in", dest);
execSync("npm ci --omit=dev", { cwd: dest, stdio: "inherit" });
console.log("[bundle-server] OK →", dest);
