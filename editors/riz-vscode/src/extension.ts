import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

function resolveLspRoot(context: vscode.ExtensionContext): { lspRoot: string; serverJs: string } | undefined {
  const cfg = vscode.workspace.getConfiguration("riz");
  const custom = (cfg.get<string>("lspServerScript") || "").trim();
  if (custom) {
    const abs = path.isAbsolute(custom) ? custom : path.resolve(custom);
    if (fs.existsSync(abs)) {
      const lspRoot = path.resolve(path.dirname(abs), "..");
      return { lspRoot, serverJs: path.join("out", "server.js") };
    }
  }
  const folders = vscode.workspace.workspaceFolders;
  if (folders?.length) {
    const candidate = path.join(folders[0].uri.fsPath, "lsp", "out", "server.js");
    if (fs.existsSync(candidate)) {
      const lspRoot = path.join(folders[0].uri.fsPath, "lsp");
      return { lspRoot, serverJs: path.join("out", "server.js") };
    }
  }
  const bundled = path.join(context.extensionPath, "bundled", "lsp", "out", "server.js");
  if (fs.existsSync(bundled)) {
    const lspRoot = path.join(context.extensionPath, "bundled", "lsp");
    return { lspRoot, serverJs: path.join("out", "server.js") };
  }
  return undefined;
}

export function activate(context: vscode.ExtensionContext) {
  const resolved = resolveLspRoot(context);
  if (!resolved) {
    void vscode.window.showWarningMessage(
      "Riz: LSP server not found. Build the repo (`cd lsp && npm install && npm run build`) or set **riz.lspServerScript** to `lsp/out/server.js`."
    );
    return;
  }

  const cfg = vscode.workspace.getConfiguration("riz");
  const nodePath = (cfg.get<string>("nodePath") || "").trim() || process.execPath;

  const serverOptions: ServerOptions = {
    run: {
      command: nodePath,
      args: [resolved.serverJs, "--stdio"],
      options: { cwd: resolved.lspRoot },
      transport: TransportKind.stdio,
    },
    debug: {
      command: nodePath,
      args: [resolved.serverJs, "--stdio"],
      options: { cwd: resolved.lspRoot },
      transport: TransportKind.stdio,
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "riz" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.riz"),
    },
  };

  client = new LanguageClient("rizLanguageServer", "Riz Language Server", serverOptions, clientOptions);
  context.subscriptions.push(client);
  client.start().catch((err) => {
    void vscode.window.showErrorMessage(`Riz LSP failed to start: ${String(err)}`);
  });
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
