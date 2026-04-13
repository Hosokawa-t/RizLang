# Riz — lightweight native scripting for AI tooling (v0.9.6)

Riz is a small, C-first language with a **tree-walking interpreter**, an experimental **register VM**, and an **AOT path** to standalone native binaries. It targets workflows where you want **C-level deployment** without giving up a script-shaped surface (imports, structs, `try`/`catch`/`throw`, plugins).

### Real-world performance (benchmark: 10M iterations, π-style loop)

- **CPython 3.10** (reference): ~593 ms (environment-dependent)
- **Riz AOT** (same workload): **~351 ms** (~1.7× faster in our runs)
- **Riz AOT with typed / optimized paths**: can go much lower depending on emitted C and compiler flags

Figures are illustrative; measure on your machine for serious comparisons.

## Highlights

- **Dual runtime**: interpreter for fast iteration; `riz --vm` for bytecode; `riz --aot` for emitted C + native binary.
- **Optional type hints** (`: int`, `: float`, …) for documentation and AOT-oriented codegen.
- **Plugins**: `import_native` / **`import_python`** (default `plugin_python` library name) load `.dll` / `.so` / `.dylib` — Python (`examples/python/`), tensors, LLM bridge, etc.
- **Diagnostics**: `riz check` / `riz check --strict` (NDJSON; strict fails on warnings) with **lexical scope and undefined-name checks**, **`import "…"` export collection**, and **plugin-aware heuristics** after `import_native`; **LSP** under `lsp/`, VS Code workspace under `editors/riz-vscode`.
- **Observable errors**: `debug(x, label?)` (stderr, returns `x`), `panic(msg?)` (message + call stack, `exit(1)`), and **call stack on uncaught `throw`** in the interpreter.
- **Environment helper**: `riz env doctor` / `setup` / `init` / `shell` — see `riz env --help`.

## Built-in functions (summary)

| Area | Functions |
|------|-----------|
| I/O | `print`, `input`, `read_file`, `write_file` |
| Values | `type`, `str`, `int`, `float`, `bool`, `len`, `range` |
| Collections | `append`, `pop`, `extend`, `map`, `filter`, `sorted`, `reversed`, `enumerate`, `zip`, `keys`, `values`, `has_key` |
| Strings / text | `format`, `ord`, `chr` |
| Math | `abs`, `min`, `max`, `sum`, `clamp`, `sign`, `floor`, `ceil`, `round` |
| Parallel | `cpu_count`, `parallel_sum` |
| Logic | `all`, `any` |
| Control | `assert`, `debug`, `panic`, `exit` |
| Python bridge | **`py.exec` / `py.import` / …** (global `py` dict) and legacy `py_exec`, `py_import`, … after loading the plugin |

Full quick reference in the REPL: type `help`.

## Examples (by topic)

Sample programs live under **`examples/`** by use case. See **`examples/README.md`** for copy-paste commands.

| Folder | Purpose |
|--------|---------|
| `examples/intro/` | Hello world, builtins, imports, structs, phases |
| `examples/vm/` | `riz --vm`, import smoke |
| `examples/aot/` | `riz --aot` |
| `examples/python/` | CPython plugin |
| `examples/tensor/` | Tensor / training demos, optional PyTorch plugin |
| `examples/llm/` | GGUF / llama.cpp CLI bridge |
| `examples/bench/` | Loop benchmarks (e.g. next to `pi_bench.py`) |

Quick smoke (from repo root):

```bash
./riz examples/intro/hello.riz
./riz --vm examples/vm/vm_test.riz
```

Windows (MinGW-built `riz.exe`): same paths with `.\riz.exe`.

## Recommended development environment

- **Linux / macOS**: C11 **`gcc` or `clang`**, **`make`**, POSIX shell. Build: `make` at repo root. Parse-check all examples: `bash tools/check_examples.sh`.
- **Windows (matches CI)**: **MSYS2 MinGW64** with `mingw-w64-x86_64-gcc` and `make`. Use the MinGW64 shell so paths match `gcc` in CI. Examples check: `pwsh tools/check_examples.ps1`.
- **AOT**: `gcc` on `PATH`, or bundled **`vendor\tcc\tcc.exe`** on Windows when GCC is not found. Emitted build command is produced by `main.c` together with `src/aot_runtime.c`, `src/value.c`, and other runtime sources.
- **Editor**: **Node.js 18+** for `lsp/` (`lsp/README.md`) and packaging the VS Code extension under `editors/riz-vscode`.
- **Onboarding**: in a project directory, **`riz env setup`** then `source ./.riz/activate.sh` (POSIX) or `. .\.riz\activate.ps1` (PowerShell). Optional: `eval "$(riz env shell bash)"` for a transient PATH.

## Python interoperability

Build `plugin_python` (see `examples/python/` and `build_python.bat`). Then either load by platform default or pass an explicit path:

```riz
import_python
# or: import_python "my_plugin_python.dll"
# or: import_native "plugin_python.dll"

py.exec("import math")
let py_math = py.import("math")
let pow_fn = py.getattr(py_math, "pow")
let res = py.call(pow_fn, 2, 10)
print(res)
```

The `py_*` globals remain for backward compatibility.

**Plugin authors:** `riz_plugin.h` gained optional `make_dict`, `dict_set_fn`, and `define_global`. Rebuild any native `.dll` / `.so` against the current header; older plugins that only use `register_fn` still work when the host zero-initializes the API struct.

## Native plugins and operator hooks

Plugins can register natives; the interpreter may route operators (e.g. tensor `*`) to FFI when values carry native handles. See `examples/tensor/` and `attention_test.riz`-style demos.

## AOT diagnostics

Runtime and plugin code can surface **file + line** context (see `aot_panic` / `aot_current_line` in `src/aot_runtime.c`). Example-style messages may point at paths such as `examples/tensor/attention_test.riz`.

---

# Riz 日本語ドキュメント

Riz は **C11 ランタイム**を核にしたスクリプト言語で、**インタプリタ**・**VM（実験的）**・**AOT（C 生成）**の切り替えができます。Python ブリッジは **`import_python`**（省略時は OS 既定のプラグイン名）または **`import_native "path"`** で読み込み、スクリプトからは **`py.exec` / `py.import`** などの名前空間（従来の `py_*` も利用可）で呼び出せます。

## 主な機能

- **AOT**: `riz --aot` で Riz → C → ネイティブ実行ファイル（GCC または同梱 TinyCC）。
- **Python 連携**: `examples/python/` の `plugin_python` 経由で CPython を埋め込み。
- **テンソル / PyTorch**: `examples/tensor/`（`plugin_torch.cpp` は CMake ビルド）。
- **LLM / GGUF**: `examples/llm/` と `LLAMA_INFER.md`（`plugin_llama_cli`）。
- **並列計算**: `cpu_count()` で論理CPU数、`parallel_sum(list[, workers])` で数値リストをOSスレッドで並列集計。
- **デバッグ・エラー**: `debug`（stderr に行付きで値を出し、その値を返す）、`panic`（メッセージとスタックで終了）、インタプリタでは **未捕捉の `throw` にもコールスタック**を表示。
- **環境**: **`riz env doctor` / `setup`** で依存と PATH 用スクリプトを整備。

## サンプルの場所

**`examples/README.md`** に用途別フォルダとコマンド一覧があります。ルートからの import は `examples/intro/math.riz` のように **リポジトリルートを cwd** にしたパスで解決します。

## 実行例

```bash
riz examples/intro/hello.riz
riz --vm examples/vm/vm_test.riz
riz --aot examples/aot/aot_test.riz
```

Python デモのパス例: `examples/python/python_demo.riz`（先に `plugin_python` をビルド）。`import_python` と `py.exec` / `py.import` の使い方は英語セクション **Python interoperability** のコードブロックと同じです。

**ネイティブプラグイン:** `riz_plugin.h` を更新したため、独自プラグインは現行ヘッダで再コンパイルしてください（`register_fn` のみの DLL はそのまま利用可能な場合が多いです）。

## 推奨開発環境

- **Linux / macOS**: C11 対応コンパイラ、`make`、POSIX シェル。例の一括チェック: `bash tools/check_examples.sh`。
- **Windows**: **MSYS2 MinGW64** + `gcc`/`make`（CI と同系統）。`pwsh tools/check_examples.ps1`。
- **エディタ**: `lsp/` 利用時は Node.js（`lsp/README.md`）。
- **最短セットアップ**: **`riz env setup`** → `.riz/activate.*` を読み込み。

## Testing

After building, `tools/check_examples.*` runs **`riz check --strict`** on every `examples/**/*.riz` (warnings fail the check). For local runs, `riz check file.riz` without `--strict` keeps exit code 0 when only warnings are reported. **`RIZ_CHECK_STRICT=1`** in the environment is equivalent to passing **`--strict`**.

- **Bash**: `bash tools/check_examples.sh`
- **PowerShell**: `pwsh tools/check_examples.ps1`

CI runs these scripts plus interpreter/VM smoke tests on Linux and Windows (MSYS2 MinGW64).

**日本語:** ビルド後、上記スクリプトが `riz check --strict` で全サンプルを検査します。手元では `riz check` のみなら警告は失敗にしません。`RIZ_CHECK_STRICT=1` で `--strict` と同じ扱いになります。

## Windows x64 配布 zip

`riz.exe` と **AOT 用 `src/`**、**`examples/`**（巨大モデル等は除外）、**ビルド済み LSP** などをまとめるには:

```powershell
pwsh tools/package_windows_release.ps1
```

出力: `riz-v{VERSION}-windows-x64.zip`（バージョンは `src/common.h` の `RIZ_VERSION`）。`vendor\tcc\tcc.exe` があれば同梱され、gcc なし AOT に使えます。

**CI**: Windows ジョブが成果物 **`riz-windows-x64-zip`** をアップロードします。リリースへ載せる例:

`gh release upload v0.9.6 riz-v0.9.6-windows-x64.zip --clobber`

## License

MIT License.
