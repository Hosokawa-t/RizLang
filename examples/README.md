# Riz examples (by use case)

Sample programs live under **topic folders** so you can copy a small script and grow from there.  
Run commands from the **repository root** unless noted. Import strings use paths like `examples/intro/math.riz` so they resolve when your cwd is the repo root.

| Folder | Purpose |
|--------|---------|
| **`intro/`** | First steps, builtins, `import`, structs, phase demos, native FFI (`plugin_math.c` → `math_ext.dll`) |
| **`vm/`** | Bytecode VM (`riz --vm`), `import` smoke (`pkg_vendor_lib`) |
| **`aot/`** | Ahead-of-time compile (`riz --aot`) |
| **`python/`** | CPython bridge (`plugin_python.c`) |
| **`tensor/`** | Tensor plugin (CPU dummy + training demos; PyTorch via `plugin_torch.cpp` + CMake) |
| **`llm/`** | GGUF / llama.cpp CLI bridge (`plugin_llama_cli.c`, `LLAMA_INFER.md`) |
| **`bench/`** | Performance-oriented loops (compare with `pi_bench.py` next to `pi_bench.riz`) |

Folders **`intro/`**, **`vm/`**, **`aot/`**, **`python/`**, **`tensor/`**, and **`llm/`** each ship a minimal **`starter.riz`** you can copy into your own project. **`bench/`** holds timing loops (`pi_bench.riz` and `pi_bench.py`) instead.

---

## Copy-paste quick start

**Interpreter (intro)**

```bash
riz examples/intro/starter.riz
riz examples/intro/hello.riz
```

**VM**

```bash
riz --vm examples/vm/starter.riz
riz --vm examples/vm/vm_test.riz
```

**AOT** (needs `gcc` or bundled TinyCC; keep `src/` next to your working tree as documented in the main README)

```bash
riz --aot examples/aot/starter.riz
```

**Python plugin** (build the DLL first; flags vary by platform / Python install)

```bash
# example (adjust -lpython… for your system)
gcc -shared -O2 -Isrc -o plugin_python.dll examples/python/plugin_python.c
riz examples/python/starter.riz
```

**Tensor plugin (CPU)**

```bash
gcc -shared -O2 -Isrc -o plugin_tensor.dll examples/tensor/plugin_tensor.c -lm
riz examples/tensor/starter.riz
```

**LLM / GGUF**

```bash
# Windows: see build_llama_cli_plugin.bat
riz examples/llm/starter.riz
# Full flow: examples/llm/LLAMA_INFER.md and examples/llm/infer_llama.riz
```

**Parse-check every `.riz` under `examples/`**

```bash
bash tools/check_examples.sh
# or: pwsh tools/check_examples.ps1
```

---

## 日本語メモ

- **入門**は `intro/`（`starter.riz`・`hello.riz`・`import_demo.riz` と `math.riz`）。
- **VM**は `vm/`。`vm_import_client.riz` は `examples/vm/pkg_vendor_lib.riz` を import する例です。
- **AOT**は `aot/`。**Python / テンソル / LLM** は各フォルダの `starter.riz` と README コメントが最短ルートです。
- **ベンチマーク**は `bench/`（`pi_bench.riz` と同じディレクトリに `pi_bench.py` あり）。

Built-ins の一覧はリポジトリ直下の **`README.md`** の「Built-in Functions」を参照してください。
