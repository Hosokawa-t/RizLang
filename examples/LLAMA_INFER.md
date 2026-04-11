# llama.cpp 推論ブリッジ（`plugin_llama_cli`）

Riz から **llama.cpp の `llama-cli`** を呼び出し、**GGUF モデル**でテキスト生成を行います。  
llama.cpp のソースはリポジトリに含めず、**あなたがビルドした `llama-cli`（または同名の互換 CLI）**を利用します。

## 前提

1. [llama.cpp](https://github.com/ggerganov/llama.cpp) を CMake 等でビルドし、`llama-cli` 実行ファイルを用意する。
2. 推論用の **`.gguf` モデル**を用意する（公式の量子化モデルなど）。

## プラグインのビルド（Windows）

リポジトリルートで:

```bat
build_llama_cli_plugin.bat
```

または:

```bat
gcc -shared -O2 -Isrc -o plugin_llama_cli.dll examples\plugin_llama_cli.c
```

生成された `plugin_llama_cli.dll` はルートに置き、`riz` をルートから実行する想定です。

## `llama-cli` の場所

- **PATH に通す**（推奨）、または  
- 環境変数 **`LLAMA_CLI`** に実行ファイルのフルパスを設定する（例: `C:\llama.cpp\build\bin\llama-cli.exe`）、または  
- Riz から `llama_set_cli("C:\\...\\llama-cli.exe")` を一度呼ぶ。

## Riz API

| 関数 | 説明 |
|------|------|
| `llama_set_cli(path)` | 使用する `llama-cli` のフルパスを設定する。 |
| `llama_infer(model_path, prompt, max_tokens)` | モデルパス・プロンプト・最大生成トークン数を指定し、**標準出力に出た文字列**をまとめて返す。 |

内部的には一時ファイルにプロンプトを書き、次に相当するコマンドを実行します:

```text
llama-cli -m "<model.gguf>" -f "<temp_prompt.txt>" -n <max_tokens> > "<temp_out>" 2>&1
```

`llama-cli` のバージョンによってはフラグ名が異なる場合があります。その場合は llama.cpp のドキュメントに合わせて `plugin_llama_cli.c` のコマンド行を調整してください。

## サンプル

`examples/infer_llama.riz` 内の **`model_path`** を自分の GGUF に書き換えてから:

```bat
riz examples\infer_llama.riz
```

## 制限（現状）

- **ストリーミング表示**は未対応（生成完了後にまとめて文字列が返る）。
- **`popen` は標準出力のみ取得**します。`llama-cli` が見つからない場合など、**エラーメッセージはコンソール（標準エラー）にだけ出る**ことがあります。
- 本番向けには、将来的に **llama.cpp を DLL 直リンク**する方式の方が高速・堅牢です（次段階）。
