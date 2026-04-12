import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  type InitializeResult,
  TextDocumentSyncKind,
  Diagnostic,
  DiagnosticSeverity,
  CompletionItem,
  CompletionItemKind,
  CompletionList,
  InsertTextFormat,
  Hover,
  MarkupKind,
  Location,
  Range,
  SymbolInformation,
  SymbolKind,
  SignatureHelp,
  SignatureInformation,
  ParameterInformation,
  type TextDocumentPositionParams,
  type DocumentSymbolParams,
  type WorkspaceSymbolParams,
  type CompletionParams,
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
/** Open .riz buffers (TextDocuments does not expose an iterator in all versions). */
const openBuffers = new Map<string, TextDocument>();

let rizExecutable = process.env.RIZ_PATH?.trim() || (process.platform === "win32" ? "riz.exe" : "riz");
let debounceMs = 200;
const debouncers = new Map<string, ReturnType<typeof setTimeout>>();

/* ─── Riz keywords & builtins (keep in sync with interpreter help) ─── */

const KEYWORDS = [
  "let",
  "mut",
  "fn",
  "return",
  "if",
  "else",
  "while",
  "for",
  "in",
  "true",
  "false",
  "none",
  "and",
  "or",
  "not",
  "match",
  "struct",
  "impl",
  "trait",
  "async",
  "await",
  "import",
  "import_native",
  "break",
  "continue",
  "try",
  "catch",
  "throw",
];

const BUILTIN_HELP: Record<string, string> = {
  print: "`print(a, b, ...)` — print values, newline-terminated.",
  len: "`len(x)` — length of list, string, or dict.",
  range: "`range(stop)` / `range(start, stop)` / `range(start, stop, step)` — list of integers.",
  type: "`type(x)` — type name string.",
  str: "`str(x)` — string representation.",
  int: "`int(x)` — cast from int/float/bool/string.",
  float: "`float(x)` — cast to float.",
  input: "`input(prompt?)` — read a line from stdin.",
  append: "`append(list, value)` — append in place.",
  pop: "`pop(list)` — remove and return last element.",
  abs: "`abs(x)` — absolute value (int or float).",
  min: "`min(a, b, ...)` or `min(list)`.",
  max: "`max(a, b, ...)` or `max(list)`.",
  sum: "`sum(list)` — sum of numeric list.",
  map: "`map(list, fn)` — apply callable to each element.",
  filter: "`filter(list, fn)` — keep elements where fn is truthy.",
  format: '`format("a {} c", x)` — simple `{}` placeholders.',
  sorted: "`sorted(list)` — new sorted list (numeric order).",
  reversed: "`reversed(list)` — new list in reverse order.",
  enumerate: "`enumerate(list)` — list of `[index, item]` pairs.",
  zip: "`zip(listA, listB)` — list of `[a, b]` pairs.",
  keys: "`keys(dict)` — list of keys.",
  values: "`values(dict)` — list of values.",
  assert: "`assert(cond, message?)` — runtime check.",
  exit: "`exit(code?)` — terminate process.",
  read_file: "`read_file(path)` — file as string.",
  write_file: "`write_file(path, content)` — bool success.",
  has_key: "`has_key(dict, keyStr)` — bool.",
  clamp: "`clamp(value, lo, hi)` — clamp numeric.",
  sign: "`sign(x)` — -1, 0, or 1.",
  floor: "`floor(x)` — floor (float stays float).",
  ceil: "`ceil(x)` — ceiling.",
  round: "`round(x)` — nearest float (C `round`).",
  all: "`all(list)` — true if every element truthy (empty: true).",
  any: "`any(list)` — true if any truthy.",
  bool: "`bool(x)` — truthiness as bool.",
  ord: "`ord(s)` — code unit of single-char string.",
  chr: "`chr(n)` — one-char string for n in 0..255.",
  extend: "`extend(list, other)` — append all items from other.",
};

const BUILTIN_SIGNATURES: Record<string, { label: string; doc?: string; params?: ParameterInformation[] }> = {
  print: { label: "print(...values)", params: [{ label: "…values" }] },
  len: { label: "len(obj)", params: [{ label: "obj" }] },
  range: { label: "range(stop) | range(start, stop) | range(start, stop, step)", params: [{ label: "…" }] },
  type: { label: "type(value)", params: [{ label: "value" }] },
  str: { label: "str(value)", params: [{ label: "value" }] },
  int: { label: "int(value)", params: [{ label: "value" }] },
  float: { label: "float(value)", params: [{ label: "value" }] },
  input: { label: "input(prompt?)", params: [{ label: "prompt?" }] },
  map: { label: "map(list, fn)", params: [{ label: "list" }, { label: "fn" }] },
  filter: { label: "filter(list, fn)", params: [{ label: "list" }, { label: "fn" }] },
  clamp: { label: "clamp(value, lo, hi)", params: [{ label: "value" }, { label: "lo" }, { label: "hi" }] },
};

async function refreshFromWorkspaceConfig(): Promise<void> {
  try {
    const cfg = await connection.workspace.getConfiguration("riz");
    const p = cfg.get("executablePath") as string | undefined;
    if (p && typeof p === "string" && p.trim()) rizExecutable = p.trim();
    else rizExecutable = process.env.RIZ_PATH?.trim() || (process.platform === "win32" ? "riz.exe" : "riz");
    const d = cfg.get("diagnosticDelay") as number | undefined;
    if (typeof d === "number" && d >= 0 && d < 60000) debounceMs = d;
    else debounceMs = 200;
  } catch {
    rizExecutable = process.env.RIZ_PATH?.trim() || (process.platform === "win32" ? "riz.exe" : "riz");
    debounceMs = 200;
  }
}

connection.onInitialize((): InitializeResult => {
  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: {
        resolveProvider: false,
        triggerCharacters: [".", "(", '"', "'", "_", "#"],
      },
      hoverProvider: true,
      definitionProvider: true,
      documentSymbolProvider: true,
      workspaceSymbolProvider: true,
      signatureHelpProvider: {
        triggerCharacters: ["(", ","],
        retriggerCharacters: [","],
      },
    },
  };
});

connection.onDidChangeConfiguration(() => {
  void refreshFromWorkspaceConfig();
});

interface RizDiagJson {
  line: number;
  message: string;
  source?: string;
  file?: string;
  startColumn?: number;
  endColumn?: number;
}

function ndjsonLineToDiagnostic(doc: TextDocument, o: RizDiagJson): Diagnostic {
  const ln = Math.max(0, o.line - 1);
  const lastLine = Math.max(0, doc.lineCount - 1);
  const line = Math.min(ln, lastLine);
  const lineText = doc.getText({
    start: { line, character: 0 },
    end: { line: line + 1, character: 0 },
  });
  const lineLen = Math.max(0, lineText.replace(/\r?\n$/, "").length);
  let startChar = 0;
  let endChar = lineLen > 0 ? lineLen : 1;
  if (typeof o.startColumn === "number" && o.startColumn >= 0) {
    startChar = Math.min(o.startColumn, Math.max(0, lineLen - 1));
    if (typeof o.endColumn === "number" && o.endColumn > startChar) {
      endChar = Math.min(o.endColumn, lineLen > 0 ? lineLen : startChar + 1);
    } else {
      endChar = Math.min(startChar + 1, lineLen > 0 ? lineLen : startChar + 1);
    }
  }
  return {
    severity: DiagnosticSeverity.Error,
    range: Range.create(line, startChar, line, endChar),
    message: o.message,
    source: o.source || "riz-parse",
  };
}

async function validateDocument(doc: TextDocument): Promise<Diagnostic[]> {
  await refreshFromWorkspaceConfig();
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
        const o = JSON.parse(t) as RizDiagJson;
        if (typeof o.line !== "number" || typeof o.message !== "string") continue;
        out.push(ndjsonLineToDiagnostic(doc, o));
      } catch {
        /* ignore */
      }
    }
    return out;
  } catch (e) {
    connection.console.log(`riz check: ${String(e)}`);
    return [
      {
        severity: DiagnosticSeverity.Warning,
        range: Range.create(0, 0, 0, 1),
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
  openBuffers.set(e.document.uri, e.document);
  const diagnostics = await validateDocument(e.document);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics });
});

documents.onDidChangeContent((change) => {
  scheduleValidate(change.document);
});

documents.onDidSave(async (e) => {
  const diagnostics = await validateDocument(e.document);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics });
});

documents.onDidClose((e) => {
  debouncers.delete(e.document.uri);
  openBuffers.delete(e.document.uri);
  connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

/* ─── Word at position ─── */

function wordAt(doc: TextDocument, pos: { line: number; character: number }): string {
  const line = doc.getText(Range.create(pos.line, 0, pos.line + 1, 0)).replace(/\r?\n$/, "");
  const ch = pos.character;
  const isId = (c: string) => /[a-zA-Z0-9_]/.test(c);
  if (ch < 0 || ch > line.length) return "";
  let s = ch;
  let e = ch;
  while (s > 0 && isId(line[s - 1])) s--;
  while (e < line.length && isId(line[e])) e++;
  return line.slice(s, e);
}

function identifierBeforeParen(doc: TextDocument, pos: { line: number; character: number }): string | null {
  const line = doc.getText(Range.create(pos.line, 0, pos.line + 1, 0)).replace(/\r?\n$/, "");
  let i = Math.min(pos.character, line.length) - 1;
  while (i >= 0 && /\s/.test(line[i])) i--;
  if (i < 0 || line[i] !== "(") return null;
  i--;
  while (i >= 0 && /\s/.test(line[i])) i--;
  let e = i + 1;
  while (i >= 0 && /[a-zA-Z0-9_]/.test(line[i])) i--;
  const name = line.slice(i + 1, e);
  return name.length ? name : null;
}

/* ─── Scan document for symbols / definitions ─── */

type DefKind = "fn" | "let" | "mut" | "struct" | "trait" | "impl";

interface NameLoc {
  name: string;
  kind: DefKind;
  range: Range;
  line: number;
}

function scanDocumentNames(text: string): NameLoc[] {
  const lines = text.split(/\r?\n/);
  const out: NameLoc[] = [];
  const push = (kind: DefKind, name: string, line: number, startCol: number, endCol: number) => {
    out.push({
      name,
      kind,
      range: Range.create(line, startCol, line, endCol),
      line,
    });
  };

  for (let i = 0; i < lines.length; i++) {
    const L = lines[i]!;
    const trim = L.trimStart();
    const indent = L.length - trim.length;

    let m = trim.match(/^fn\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:\(|\{|\=>)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("fn", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^struct\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("struct", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^trait\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("trait", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^impl\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("impl", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^let\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("let", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^mut\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, indent);
      if (idx >= 0) push("mut", name, i, idx, idx + name.length);
      continue;
    }
    m = trim.match(/^for\s+([a-zA-Z_][a-zA-Z0-9_]*)\s+in/);
    if (m) {
      const name = m[1]!;
      const idx = L.indexOf(name, L.indexOf("for"));
      if (idx >= 0) push("let", name, i, idx, idx + name.length);
    }
  }
  return out;
}

function symbolKindFor(k: DefKind): SymbolKind {
  switch (k) {
    case "fn":
      return SymbolKind.Function;
    case "struct":
    case "trait":
    case "impl":
      return SymbolKind.Class;
    default:
      return SymbolKind.Variable;
  }
}

connection.onDocumentSymbol((params: DocumentSymbolParams): SymbolInformation[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const names = scanDocumentNames(doc.getText());
  return names.map(
    (n) =>
      ({
        name: n.name,
        kind: symbolKindFor(n.kind),
        location: Location.create(doc.uri, n.range),
        containerName: n.kind,
      }) as SymbolInformation
  );
});

connection.onDefinition((params: TextDocumentPositionParams): Location | Location[] | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const w = wordAt(doc, params.position);
  if (!w) return null;
  const names = scanDocumentNames(doc.getText());
  const hit = names.filter((n) => n.name === w);
  if (hit.length === 0) return null;
  /* Prefer fn over let if duplicate names */
  const fnFirst = hit.find((h) => h.kind === "fn") || hit[0]!;
  return Location.create(doc.uri, fnFirst.range);
});

connection.onWorkspaceSymbol(async (params: WorkspaceSymbolParams): Promise<SymbolInformation[]> => {
  const q = (params.query || "").trim().toLowerCase();
  const out: SymbolInformation[] = [];
  for (const doc of openBuffers.values()) {
    if (!doc.uri.endsWith(".riz")) continue;
    for (const n of scanDocumentNames(doc.getText())) {
      if (!q || n.name.toLowerCase().includes(q)) {
        out.push({
          name: n.name,
          kind: symbolKindFor(n.kind),
          location: Location.create(doc.uri, n.range),
          containerName: path.basename(doc.uri),
        });
      }
    }
  }
  return out.slice(0, 200);
});

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const w = wordAt(doc, params.position);
  if (!w) return null;
  if (BUILTIN_HELP[w]) {
    return {
      contents: { kind: MarkupKind.Markdown, value: `### builtin \`${w}\`\n\n${BUILTIN_HELP[w]}` },
    };
  }
  if (KEYWORDS.includes(w)) {
    return {
      contents: { kind: MarkupKind.Markdown, value: `### keyword \`${w}\`\n\nRiz reserved word.` },
    };
  }
  const names = scanDocumentNames(doc.getText());
  const defs = names.filter((n) => n.name === w);
  if (defs.length) {
    const kinds = [...new Set(defs.map((d) => d.kind))].join(", ");
    return {
      contents: {
        kind: MarkupKind.Markdown,
        value: `### \`${w}\`\n\nDefined in this file as: **${kinds}**`,
      },
    };
  }
  return null;
});

connection.onCompletion((params: CompletionParams): CompletionList => {
  const doc = params.textDocument ? documents.get(params.textDocument.uri) : undefined;
  const pos = params.position;
  let prefix = "";
  if (doc && pos) {
    const line = doc.getText(Range.create(pos.line, 0, pos.line + 1, 0)).replace(/\r?\n$/, "");
    let s = Math.min(pos.character, line.length);
    while (s > 0 && /[a-zA-Z0-9_]/.test(line[s - 1])) s--;
    prefix = line.slice(s, Math.min(pos.character, line.length)).toLowerCase();
  }

  const items: CompletionItem[] = [];

  const match = (label: string) => !prefix || label.toLowerCase().startsWith(prefix);

  for (const k of KEYWORDS) {
    if (!match(k)) continue;
    items.push({
      label: k,
      kind: CompletionItemKind.Keyword,
      sortText: `0_${k}`,
    });
  }

  for (const b of Object.keys(BUILTIN_HELP)) {
    if (!match(b)) continue;
    items.push({
      label: b,
      kind: CompletionItemKind.Function,
      detail: "builtin",
      documentation: BUILTIN_HELP[b],
      sortText: `1_${b}`,
    });
  }

  /* Snippets use distinct labels so they do not collide with keyword completions. */
  if (!prefix || prefix.length <= 2) {
    items.push(
      {
        label: "fn … { }",
        kind: CompletionItemKind.Snippet,
        insertText: "fn ${1:name}(${2}) {\n\t${0}\n}",
        insertTextFormat: InsertTextFormat.Snippet,
        detail: "snippet",
      },
      {
        label: "let … =",
        kind: CompletionItemKind.Snippet,
        insertText: "let ${1:name} = ${0}",
        insertTextFormat: InsertTextFormat.Snippet,
        detail: "snippet",
      },
      {
        label: "for … in",
        kind: CompletionItemKind.Snippet,
        insertText: "for ${1:i} in ${2:range(10)} {\n\t${0}\n}",
        insertTextFormat: InsertTextFormat.Snippet,
        detail: "snippet",
      },
      {
        label: "if … { }",
        kind: CompletionItemKind.Snippet,
        insertText: "if ${1:cond} {\n\t${0}\n}",
        insertTextFormat: InsertTextFormat.Snippet,
        detail: "snippet",
      },
      {
        label: "struct … { }",
        kind: CompletionItemKind.Snippet,
        insertText: "struct ${1:Name} {\n\t${2:field}: ${3:int}\n}",
        insertTextFormat: InsertTextFormat.Snippet,
        detail: "snippet",
      }
    );
  }

  return { isIncomplete: false, items };
});

connection.onSignatureHelp((params: TextDocumentPositionParams): SignatureHelp | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const callee = identifierBeforeParen(doc, params.position);
  if (!callee || !BUILTIN_SIGNATURES[callee]) return null;
  const meta = BUILTIN_SIGNATURES[callee]!;
  const sig = SignatureInformation.create(meta.label, meta.doc, ...(meta.params || []));
  const line = doc.getText(Range.create(params.position.line, 0, params.position.line + 1, 0)).replace(/\r?\n$/, "");
  const before = line.slice(0, Math.min(params.position.character, line.length));
  const needle = callee + "(";
  const callIdx = before.lastIndexOf(needle);
  let activeParameter = 0;
  if (callIdx >= 0) {
    const inner = before.slice(callIdx + needle.length);
    let depth = 0;
    for (const ch of inner) {
      if (ch === "(") depth++;
      else if (ch === ")") depth--;
      else if (ch === "," && depth === 0) activeParameter++;
    }
  }
  return {
    signatures: [sig],
    activeSignature: 0,
    activeParameter: Math.min(activeParameter, Math.max(0, (meta.params?.length || 1) - 1)),
  };
});

documents.listen(connection);
connection.listen();
