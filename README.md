# Riz Programming Language

Riz is a high-performance, statically typed yet dynamically executed programming language designed for AI development, offering a clean syntax inspired by Modern languages. It prioritizes speed, minimal dependencies, and native interoperability.

## Features

- **Blazing Fast Execution**: Operates across three execution modes:
  - **AST Interpreter**: Direct evaluation for rapid prototyping.
  - **Register VM**: High-performance bytecode virtual machine utilizing a register-based architecture and computed goto dispatch for optimal CPU branch prediction.
  - **AOT Compiler**: Ahead-of-time compilation that transpiles Riz code directly to C and leverages GCC to produce standalone native binaries.
- **Native FFI**: Seamless interaction with C/C++ libraries via `import_native`.
- **Zero Overhead Portability**: Compiles down to lightweight native executables (as small as a few hundred kilobytes) without requiring a heavy runtime environment.

## Usage

### Building from Source

Ensure you have GCC installed (e.g., via MSYS2 MinGW-w64 on Windows):

```bash
gcc -Wall -O2 -std=c11 -Isrc -o riz src/*.c -lm
```

### Running Code

Executes the script via the standard AST interpreter:
```bash
riz example.riz
```

Runs the script using the high-performance Register VM:
```bash
riz --vm example.riz
```

Ahead-of-Time compilation: Transpiles the script to C and generates a standalone native executable:
```bash
riz --aot example.riz
./example.exe
```

## Architecture

Riz separates execution into a front-end (Lexer, Parser, AST) and multiple back-ends. The recent addition of a Register-based VM and an AOT transpilation pipeline allows Riz scripts to execute faster than conventional interpreted languages like Python while maintaining script-like development agility.

---

# Riz プログラミング言語 (Japanese)

Rizは、モダンな構文を持ち、AI開発に向けた高いパフォーマンスとネイティブコードとの連携を重視したプログラミング言語です。速度、最小限の依存関係、ポータビリティに重点を置いて設計されています。

## 主な機能

- **超高速な実行速度**: 3つの実行モードを備えています:
  - **ASTインタプリタ**: 速やかなプロトタイピングに適した直接評価。
  - **レジスタVM**: レジスタベースのアーキテクチャと Computed Goto ディスパッチを採用し、CPUの分岐予測を最適化した高性能バイトコード仮想マシン。
  - **AOTコンパイラ**: 事前コンパイル機能。Rizのコードを高速にC言語へ変換し、GCCを利用してオーバーヘッドのない数十〜数百KBのスタンドアロン・ネイティブバイナリ(.exe等)を生成します。
- **ネイティブ FFI**: `import_native` 構文により、C/C++等の外部共有ライブラリ(DLL/SO)をシームレスに直接呼び出すことが可能です。
- **軽量なフットプリント**: 言語のランタイム自体が極めて小さく、専用の重い実行環境を構築することなく配布・実行が可能です。

## 使用方法

### ビルド

GCCがインストールされた環境（Windowsの場合は MSYS2 MinGW-w64 など）でビルドします:

```bash
gcc -Wall -O2 -std=c11 -Isrc -o riz src/*.c -lm
```

### スクリプトの実行

通常のASTインタプリタで実行:
```bash
riz example.riz
```

最適化されたレジスタVMで実行:
```bash
riz --vm example.riz
```

事前コンパイル (AOT) でネイティブバイナリを生成して実行:
```bash
riz --aot example.riz
./example.exe
```

## アーキテクチャ

Rizはフロントエンド（字句解析、構文解析、抽象構文木）と複数のバックエンドに処理が分離されています。新たに実装されたレジスタVMとAOTコンパイラにより、スクリプト言語のような手軽さを保ちながら、従来のインタプリタ言語（Pythonなど）を大幅に凌駕する実行速度を実現しています。
