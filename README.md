# Riz: The Lightweight Native AI Engine (v0.9.2)

Riz is a high-performance, minimalist programming language designed for AI development, featuring a tree-walking interpreter, a VM, and an ultra-fast AOT (Ahead-of-Time) compiler. It is built to bridge the gap between Python's flexible ecosystem and C's native execution speed.

### Real-world Performance (Benchmark: 10M iterations Pi calculation)
*   🐍 **Pure Python (3.10)**: 593ms
*   ⚡ **Riz AOT (v0.9.2)**: **351ms** (~1.7x faster)
*   🔥 **Riz AOT (Typed Optimization)**: Under 50ms (Up to 10x faster than Python)

## 🌟 Key Features
- **Zero-Dependency AOT**: Bundles TCC (Tiny C Compiler) to generate `.exe` without pre-installed tools.
- **Static Typing**: Optional `: int` or `: float` annotations for maximum performance.
- **Python Bridge**: Call any Python/Torch function directly with `py_call`.
- **LLM/GGUF Native Support**: Run 7B+ models via `plugin_llama_cli` with GPU (CUDA) acceleration.
- **Embedded REPL**: Interactive environment for quick prototyping.
- **Frictionless environment (`riz env`)**: `riz env doctor` audits gcc/git/curl and optional bundled TCC; `riz env setup` creates `riz.json` + `vendor/` when missing, runs `riz pkg install`, and writes `.riz/activate.sh` / `.riz/activate.ps1` so you avoid hand-tuned PATH and “venv-like” ceremony.

## ⌨️ Built-in Functions
- `print(...)`: Print values to console.
- `input(prompt?)`: Wait for user input (useful to prevent AOT windows from closing).
- `range(n)`, `len(obj)`, `type(obj)`, `str` / `int` / `float` casts, `abs`, `min`, `max`, `sum`.
- Collections / helpers: `append`, `pop`, `map`, `filter`, `format`, `sorted`, `reversed`, `enumerate`, `zip`, `keys`, `values`, `has_key`, `extend(list, other)`.
- Control / I/O: `assert`, `exit`, `read_file`, `write_file`.
- Numeric / logic: `clamp(value, lo, hi)`, `sign(x)`, `floor`, `ceil`, `round`, `all(list)`, `any(list)`, `bool(x)`.
- Characters: `ord("A")` (single-character string), `chr(0..255)` (byte to one-character string).
- `py_import("module")`, `py_call(fn, ...)` (requires Python plugin).

## Recommended development environment

- **Linux / macOS**: `gcc` or `clang` supporting **C11**, GNU `make`, standard POSIX shell. Build from the repo root with `make`; run examples with `./riz examples/intro/hello.riz` and `./riz --vm examples/vm/vm_test.riz`.
- **Windows (recommended, matches CI)**: **MSYS2** with the **MinGW64** toolchain (`pacman -S mingw-w64-x86_64-gcc make`). Build and run from an **MSYS2 MinGW64** shell so paths and `gcc` match the Linux-oriented scripts. Optional: **Git for Windows** bash if you use `tools/check_examples.sh` instead of PowerShell.
- **AOT (`riz --aot`)**: requires **`gcc` on PATH** (or bundled `vendor\tcc\tcc.exe` on Windows when GCC is not detected). The generated C is compiled together with `src/aot_runtime.c`, `src/value.c`, and other runtime sources as emitted by `main.c`.
- **Editor tooling**: **Node.js** if you use the minimal LSP under `lsp/` (see `lsp/README.md`) or the `editors/riz-vscode` workspace.
- After building `riz` / `riz.exe`, validate parse diagnostics on all examples: `bash tools/check_examples.sh` or `pwsh tools/check_examples.ps1`.
- **Fastest onboarding**: in any empty project folder, run **`riz env setup`** once, then `source ./.riz/activate.sh` (POSIX) or **`. .\.riz\activate.ps1`** (PowerShell). Use **`riz env shell bash`** with `eval "$(riz env shell bash)"` if you prefer not to keep helper files.

### 1. Dual Engine: Interpreter & AOT Transpiler
- **AST Interpreter**: Direct evaluation for rapid prototyping and CLI testing.
- **Register VM**: High-performance bytecode virtual machine.
- **Zero-Cost AOT Compiler**: Compiles Riz scripts strictly down to `C11`, utilizing `GCC` to compile standalone Native Executables. AOT emits exact memory assignments avoiding GC pausing, rivaling pure C++ speed.

### 2. CPython Interoperability (Mojo Rivaling)
Riz embraces the entire Python data science ecosystem directly from `C`. Through the `plugin_python.dll` API Bridge, Riz loads a standalone CPython core, allowing you to fluidly write Riz scripts that invoke Python code, seamlessly converting variables, Tuples and Lists automatically.

```python
# python_demo.riz
import_native "plugin_python.dll"

# Execute standard Python imports
py_exec("import numpy as np")
py_exec("print('Hello from the CPython core!')")

# Fetch an existing Python Module and call it directly from Riz
let py_math = py_import("math")
let pow_fn = py_getattr(py_math, "pow")

let res = py_call(pow_fn, 2, 10)  # Natively calculates `math.pow(2, 10)`
print(res) # 1024
```

### 3. Native Operator Overloading via Neural Plugins
Riz supports operator overloading out of the box dynamically via plugins. Want to multiply PyTorch Tensors? Riz intercepts `.riz` math operators `X * W1` and redirects them straight to the `LibTorch` C++ DLL bypassing interpreted overhead. 

```python
# attention.riz
import_native "plugin_torch.dll"

fn attention(X, W) {
    # Dynamically maps `*` to `torch::matmul(X, W)` in C++ Library
    let z = X * W  
    return tensor_relu(z)
}
```

### 4. Rust-Style Error Diagnostics
AI errors like Tensor Shape Mismatches normally crash C++ completely yielding hundreds of obscure standard library errors. Riz embeds safe boundaries and dynamic line tracers to isolate identical code segments visually across AOT Compilation.

```text
[Riz AI Panic] PyTorch Error in 'tensor_matmul':
    mat1 and mat2 shapes cannot be multiplied (2x4 and 8x4)
  --> examples\attention_test.riz:10

   8 | 
   9 | fn attention(x, W) {
  10 |     let z = x * W
     | ^^^^^^
```

---

# Riz 日本語ドキュメント

Riz は、最新のAI開発に向けて設計されたC言語連携・超高速スクリプト言語です。
「MojoのようにPython系資産を活用しながら、極小のC言語エンジンで動く」というアプローチに加え、LLM（Large Language Models）のネイティブ推論サポートを強化しています。

## 🔥 メイン機能

- **AOTコンパイラ (超速C言語直訳)**
  スクリプトを記述後、`--aot` フラグ一つでRizコードを 100% ピュアな C11言語に自動翻訳 (トランスパイル) し、GCCで直接 `exe` にコンパイルします。不要なGCなどが発生しないため、最速で実行可能です。
- **Pythonの完全な相互運用 (`plugin_python`)**
  コンパイルされた数百KBの実行ファイルでありながら、バックグラウンドにCPython環境をシームレスに結合し、Rizの中から `python` の関数、`numpy`・`matplotlib` 等を直接呼び出して変数をやり取りできます。
- **直感的な深層学習とC++プラグイン (`plugin_torch`)**
  プラグインを通じて演算子（`*` や `+`）を自由にオーバーロード可能。テンソル（PyTorchライブラリ）等のC++の数式演算を、Pythonのように直感的に記述できます。
- **LLMネイティブ推論 (`plugin_llama_cli`)**
  llama.cppをベースにしたプラグインにより、GGUF形式のLLMをGPU（NVIDIA/AMD）で高速に推論可能。Rizから数行でAIチャット機能を組み込めます。
- **Rustスタイルのパニック表示**
  C++側のバグや行列の次元数エラー（Shape Mismatch）が起きた場合、C++のスタックトレースでシステムごと落ちるのではなく、Rizのソースコードの何行目が原因か、ソースコードをターミナルに赤色で展開してRustのように美しく表示します。
- **環境まわりを簡略化 (`riz env`)**
  Python の venv / pyenv のように多段の手順を取らず、**`riz env setup`** で `riz.json` と `vendor/` の用意・依存の `install`・PATH 用の **`.riz/activate.sh` / `.riz/activate.ps1`** まで一気に生成。診断だけなら **`riz env doctor`**。

## 実行方法

AOT（自動コンパイルモード）で実行：
```bash
riz --aot examples\python_demo.riz
examples\python_demo.exe
```

## 推奨開発環境

- **Linux / macOS**: **C11** 対応の `gcc` または `clang`、`make`、POSIX シェル。リポジトリルートで `make` を実行し、`./riz` や `./riz --vm` でサンプルを確認できます。
- **Windows（CI と揃えるなら）**: **MSYS2** の **MinGW64** 環境（`mingw-w64-x86_64-gcc` と `make`）。**MSYS2 MinGW64** ターミナルでビルド・実行すると、Linux 向け手順との差が最小になります。
- **AOT**: `gcc` が PATH にあること（Windows で見つからない場合は同梱の `vendor\tcc\tcc.exe` にフォールバック）。生成 C とランタイムは `main.c` 内のコマンドラインどおりにまとめてコンパイルされます。
- **エディタ**: `lsp/` や `editors/riz-vscode` を使う場合は **Node.js** を用意してください（`lsp/README.md` 参照）。
- 例題の構文チェック: `bash tools/check_examples.sh` または `pwsh tools/check_examples.ps1`（ビルド済みの `riz` / `riz.exe` が必要）。
- **最短の始め方**: 空の作業ディレクトリで **`riz env setup`** → POSIX なら **`source ./.riz/activate.sh`**、PowerShell なら **`. .\.riz\activate.ps1`**。ファイルを増やしたくない場合は **`eval "$(riz env shell bash)"`**（bash 系）。

## Testing

After building `riz` / `riz.exe`, run a parse-only sweep over every file in `examples/`:

- **Linux / macOS / Git Bash:** `bash tools/check_examples.sh`
- **Windows (PowerShell):** `pwsh tools/check_examples.ps1`

CI runs the same check on Linux and on **Windows (MSYS2 MinGW64)** so the core C toolchain stays green on both platforms.

## Windows x64 bundle (zip) / Windows 配布用 zip

Pack **riz.exe** with **AOT `src/`**, **examples** (`.riz` / docs only; large models and build artifacts are excluded), **LSP** (`lsp/out` + pruned `node_modules`), and **README-WINDOWS.md**:

```powershell
pwsh tools/package_windows_release.ps1
```

Output: `riz-v{VERSION}-windows-x64.zip` beside `riz.exe` (version from `src/common.h`). If **`vendor\tcc\tcc.exe`** exists in the repo, it is included for AOT without gcc.

**CI:** the Windows job uploads this zip as artifact **`riz-windows-x64-zip`**. Attach to a GitHub release, e.g. `gh release upload v0.9.3 riz-v0.9.3-windows-x64.zip --clobber`.

同じ内容を **GitHub Actions の Windows ジョブ**が成果物 **`riz-windows-x64-zip`** として保存します。リリースに載せる例は上記 `gh release upload` です。

## License

MIT License.
