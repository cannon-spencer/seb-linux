#!/usr/bin/env bash
# Run on the Orange Pi (bookworm / riscv64). Writes ~/safe-exam-browser.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/safe-exam-browser}"

export DEBIAN_FRONTEND=noninteractive

echo "apt build deps"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential ca-certificates file \
  qmake6 qt6-base-dev qt6-tools-dev-tools libqt6svg6-dev \
  libwebkit2gtk-4.1-dev libgtk-3-dev \
  libssl-dev libtiff-dev libxcb-cursor0 zlib1g-dev

echo "build WebKitGTK"
"$ROOT/scripts/build.sh" CONFIG+=force_webkitgtk

BIN="$ROOT/build/bin/safe-exam-browser"
[[ -x "$BIN" ]] || { echo "error: missing $BIN" >&2; exit 1; }
file "$BIN"
file "$BIN" | grep -qiE 'riscv64|RISC-V' \
  || { echo "error: binary is not riscv64" >&2; exit 1; }

cp "$BIN" "$OUT"
chmod +x "$OUT"
echo "wrote $OUT"
