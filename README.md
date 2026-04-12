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

## ⌨️ Built-in Functions
- `print(...)`: Print values to console.
- `input(prompt?)`: Wait for user input (useful to prevent AOT windows from closing).
- `range(n)`, `len(obj)`, `type(obj)`, etc.
- `py_import("module")`, `py_call(fn, ...)` (requires Python plugin).

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

## 実行方法

AOT（自動コンパイルモード）で実行：
```bash
riz --aot examples\python_demo.riz
examples\python_demo.exe
```

## Testing

After building `riz` / `riz.exe`, run a parse-only sweep over every file in `examples/`:

- **Linux / macOS / Git Bash:** `bash tools/check_examples.sh`
- **Windows (PowerShell):** `pwsh tools/check_examples.ps1`

CI runs the same check on Linux and on **Windows (MSYS2 MinGW64)** so the core C toolchain stays green on both platforms.

## License

MIT License.
