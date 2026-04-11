# Riz for VS Code / Cursor

Syntax highlighting for `.riz` and the **Riz Language Server** (`riz check` diagnostics).

The LSP is loaded from (in order):

1. **`riz.lspServerScript`** — if you set an absolute path to `server.js`
2. **Extension Development (F5)** — workspace `lsp/out/server.js` when present, else **bundled** `bundled/lsp/`
3. **Installed VSIX (Production)** — **`bundled/lsp/`** first, then workspace `lsp/`

## Prereqs for end users

- **Node.js** (same as VS Code / Cursor; used to run the bundled LSP).
- **`riz` / `riz.exe` on `PATH`**, or set **`riz.executablePath`** in settings (used for `riz check`).

## Develop this extension

1. Repo root: `cd lsp && npm ci && npm run build`
2. Open **`editors/riz-vscode`** (or multi-root with the repo).
3. `npm ci && npm run compile`
4. **Run → Run Riz Extension** (F5).

## Pack a self-contained VSIX (配布用)

先に **LSP をビルド** して `lsp/out/server.js` を用意します（リポジトリルートから）:

```bash
cd lsp && npm ci && npm run build
cd ../editors/riz-vscode && npm ci && npm run package
```

`npm run package`（内部は `vsce package --no-dependencies`）の直前に **`vscode:prepublish`** が走り、**`npm run compile`** と **`node scripts/bundle-server.cjs`** が実行されます。後者が `lsp/out/server.js` と本番用 `node_modules` を **`bundled/lsp/`** にまとめます。`bundled/` と生成された **`*.vsix`** は `.gitignore` 対象のため **git には載りません** が、ローカルまたは CI で作った VSIX にはすべて含まれます。

生成例: `riz-0.1.0.vsix`（`editors/riz-vscode/` に出力）

**CI:** `main` / `release` への push と `main` 向け PR で、GitHub Actions が同様に VSIX を組み立て、**Artifacts** に `riz-vscode-vsix` としてアップロードします（該当ワークフロー Run からダウンロード可能）。

### インストール（VS Code / Cursor 共通）

1. 拡張ビュー → **`…`（Views and More Actions）** → **Install from VSIX…**
2. 上記 `.vsix` を選択 → 再読み込み

### 配布チャネル（おすすめ順）

| 方法 | 手順の要点 |
|------|------------|
| **GitHub Releases** | Release に `.vsix` を添付。README に「VSIX を入れて `riz.executablePath` を設定」と一行で書く。 |
| **Open VSX** | アカウント作成 → [Publishing](https://github.com/eclipse/openvsx/wiki/Publishing-Extensions) に従い `npx ovsx publish riz-0.1.0.vsix -p <token>`。Cursor は Open VSX を参照する。 |
| **Visual Studio Marketplace** | 下記「[Visual Studio Marketplace 公開手順](#visual-studio-marketplace-公開手順)」。 |
| **社内 / チームのみ** | `.vsix` を共有ドライブや社内ポータルに置き、上と同じ **Install from VSIX** で配る。 |

**注意:** VSIX には **インタプリタ本体（`riz`）は入っていません**。利用者は別途 `riz` をビルド／配布するか、`riz.executablePath` で zip 展開先の `riz.exe` を指します。

### Visual Studio Marketplace 公開手順

公式ガイド: [Publishing Extensions](https://code.visualstudio.com/api/working-with-extensions/publishing-extension)

1. **Publisher を作る**  
   [Visual Studio Marketplace の管理](https://marketplace.visualstudio.com/manage) にサインインし、**Create publisher** で **Publisher ID**（`package.json` の `"publisher"` と一致させる）と表示名を登録する。

2. **Personal Access Token (PAT)**  
   [Azure DevOps](https://dev.azure.com) にサインイン → 右上 **User settings** → **Personal access tokens** → **New Token**。  
   **Scopes** で **Marketplace** の **Manage** にチェックを入れて作成し、表示されたトークンをコピーする（再表示されない）。

3. **`package.json` を合わせる**  
   `"publisher"` を手順 1 の Publisher ID に変更する。`README`・`repository`・アイコン（任意）・カテゴリは [extension manifest](https://code.visualstudio.com/api/references/extension-manifest) の要件を満たす。

4. **ログイン（1 回 / マシン）**  
   `editors/riz-vscode` で:

   ```bash
   npx @vscode/vsce login thosokawa
   ```

   プロンプトに PAT を貼り付ける。

5. **ビルドして公開**  
   LSP をビルドしたうえで（上記「Pack a self-contained VSIX」と同じ前提）:

   ```bash
   cd lsp && npm ci && npm run build
   cd ../editors/riz-vscode && npm ci && npx @vscode/vsce publish --no-dependencies
   ```

   `vscode:prepublish` が走り、拡張の compile・LSP の bundle の後に Marketplace にアップロードされる。  
   更新のたびに **`package.json` の `version` を上げる**（セマンティックバージョニング）。同じバージョンの再公開はできない。

6. **公開後**  
   数分〜で [Marketplace の検索](https://marketplace.visualstudio.com/vscode) に反映される。初回は表示名・説明文がポリシーに沿っているか確認されることがある。

**ヒント:** CI から出す場合は PAT をシークレットに入れ、`vsce publish -p $VSCE_PAT` のように渡す（リポジトリに PAT を書かない）。

### `publisher` 名について

現在の `package.json` の **`publisher` は `thosokawa`** です。Marketplace で作った Publisher ID と違う場合はそちらに合わせて書き換えてください。

## Settings

| Key | Meaning |
|-----|--------|
| `riz.executablePath` | Path to `riz` / `riz.exe` for `riz check`. |
| `riz.lspServerScript` | Optional path to `server.js` (override search). |
| `riz.nodePath` | Optional Node binary (default: current Node). |
