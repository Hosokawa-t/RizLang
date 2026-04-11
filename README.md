# Riz Programming Language (Riz-Lang)

Riz is a high-performance, statically typed yet dynamically executed programming language designed specifically for AI and Machine Learning development. Providing a clean, Pythonic syntax directly on top of extreme C/C++ execution layers, it acts as a lightweight, zero-overhead alternative to heavy compilation toolchains like Mojo.

> **Our Philosophy**: Maximum Scripting Agility meeting Uncompromised C11 / GPU Execution Speed.

## 🚀 Key Features

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

Riz は、最新のAI開発に向けて設計されたC言語連携・超高速スクリプト言語です。「MojoのようにPython系資産を活用しながら、極小のC言語エンジンで動く」という真新しいアプローチを取っています。

## 🔥 メイン機能

- **AOTコンパイラ (超速C言語直訳)**
  スクリプトを記述後、`--aot` フラグ一つでRizコードを 100% ピュアな C11言語に自動翻訳 (トランスパイル) し、GCCで直接 `exe` にコンパイルします。不要なGCなどが発生しないため、最速で実行可能です。
- **Pythonの完全な相互運用 (`plugin_python`)**
  コンパイルされた数百KBの実行ファイルでありながら、バックグラウンドにCPython環境をシームレスに結合し、Rizの中から `python` の関数、`numpy`・`matplotlib` 等を直接呼び出して変数をやり取りできます。
- **直感的な深層学習とC++プラグイン (`plugin_torch`)**
  プラグインを通じて演算子（`*` や `+`）を自由にオーバーロード可能。テンソル（PyTorchライブラリ）等のC++の数式演算を、Pythonのように直感的に記述できます。
- **Rustスタイルのパニック表示**
  C++側のバグや行列の次元数エラー（Shape Mismatch）が起きた場合、C++のスタックトレースでシステムごと落ちるのではなく、Rizのソースコードの何行目が原因か、ソースコードをターミナルに赤色で展開してRustのように美しく表示します。

## 実行方法

AOT（自動コンパイルモード）で実行：
```bash
riz --aot examples\python_demo.riz
examples\python_demo.exe
```

## License

MIT License.
