#!/usr/bin/env bash
# Run `riz check` on every examples/**/*.riz (parse + diagnostics only).
# Usage: from repo root, after building riz:
#   bash tools/check_examples.sh
#   bash tools/check_examples.sh ./path/to/riz

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ "${1-}" != "" ]]; then
  RIZ="$1"
elif [[ -x ./riz ]]; then
  RIZ=./riz
elif [[ -f ./riz.exe ]]; then
  RIZ=./riz.exe
else
  echo "check_examples: no ./riz or ./riz.exe; build first or pass path as arg" >&2
  exit 1
fi

mapfile -t files < <(find examples -name '*.riz' -type f | sort)
if [[ ${#files[@]} -eq 0 ]]; then
  echo "check_examples: no .riz files under examples/" >&2
  exit 1
fi
for f in "${files[@]}"; do
  echo "check $f"
  "$RIZ" check "$f"
done

echo "OK: all example programs parse clean."
