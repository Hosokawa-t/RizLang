import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  type InitializeResult,
  TextDocumentSyncKind,
  Diagnostic,
  DiagnosticSeverity,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";
import { execFile } from "child_process";
import { promisify } from "util";
import * as fs from "fs/promises";
import * as os from "os";
import * as path from "path";

const execFileAsync = promisify(execFile);
const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let rizExecutable = process.env.RIZ_PATH?.trim() || (process.platform === "win32" ? "riz.exe" : "riz");
const debounceMs = 200;
const debouncers = new Map<string, ReturnType<typeof setTimeout>>();

async function refreshRizExecutableFromWorkspace(): Promise<void> {
  try {
    const cfg = await connection.workspace.getConfiguration("riz");
    const p = cfg.get("executablePath") as string | undefined;
    if (p && typeof p === "string" && p.trim()) {
      rizExecutable = p.trim();
      return;
    }
  } catch {
    /* no workspace configuration client */
  }
  rizExecutable = process.env.RIZ_PATH?.trim() || (process.platform === "win32" ? "riz.exe" : "riz");
}

connection.onInitialize((): InitializeResult => {
  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
    },
  };
});

connection.onDidChangeConfiguration(() => {
  void refreshRizExecutableFromWorkspace();
});

async function validateDocument(doc: TextDocument): Promise<Diagnostic[]> {
  await refreshRizExecutableFromWorkspace();
  const text = doc.getText();
  const safe = doc.uri.replace(/[^a-zA-Z0-9_.-]/g, "_").slice(-80);
  const tmp = path.join(os.tmpdir(), `riz-lsp-${process.pid}-${safe}.riz`);
  await fs.writeFile(tmp, text, "utf8");
  try {
    const { stdout } = await execFileAsync(rizExecutable, ["check", tmp], {
      encoding: "utf8",
      maxBuffer: 10 * 1024 * 1024,
      windowsHide: true,
    });
    const out: Diagnostic[] = [];
    for (const line of stdout.split("\n")) {
      const t = line.trim();
      if (!t) continue;
      try {
        const o = JSON.parse(t) as { line: number; message: string };
        if (typeof o.line !== "number" || typeof o.message !== "string") continue;
        const ln = Math.max(0, o.line - 1);
        const lastLine = Math.max(0, doc.lineCount - 1);
        const line = Math.min(ln, lastLine);
        const lineText = doc.getText({
          start: { line, character: 0 },
          end: { line: line + 1, character: 0 },
        });
        const endChar = Math.max(0, lineText.replace(/\r?\n$/, "").length);
        out.push({
          severity: DiagnosticSeverity.Error,
          range: {
            start: { line, character: 0 },
            end: { line, character: endChar > 0 ? endChar : 1 },
          },
          message: o.message,
          source: "riz-parse",
        });
      } catch {
        /* ignore non-JSON */
      }
    }
    return out;
  } catch (e) {
    connection.console.log(`riz check: ${String(e)}`);
    return [
      {
        severity: DiagnosticSeverity.Warning,
        range: {
          start: { line: 0, character: 0 },
          end: { line: 0, character: 1 },
        },
        message: `Could not run "${rizExecutable} check" (set riz.executablePath or RIZ_PATH).`,
        source: "riz-lsp",
      },
    ];
  } finally {
    await fs.unlink(tmp).catch(() => {});
  }
}

function scheduleValidate(doc: TextDocument) {
  const prev = debouncers.get(doc.uri);
  if (prev) clearTimeout(prev);
  debouncers.set(
    doc.uri,
    setTimeout(async () => {
      debouncers.delete(doc.uri);
      const diagnostics = await validateDocument(doc);
      connection.sendDiagnostics({ uri: doc.uri, diagnostics });
    }, debounceMs)
  );
}

documents.onDidOpen(async (e) => {
  const diagnostics = await validateDocument(e.document);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics });
});

documents.onDidChangeContent((change) => {
  scheduleValidate(change.document);
});

documents.onDidClose((e) => {
  debouncers.delete(e.document.uri);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

documents.listen(connection);
connection.listen();
