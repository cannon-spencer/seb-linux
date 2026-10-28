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

# The WebKitGTK extension (URL filter, SEB key headers, JS API) is looked up
# in webkit-extensions/ next to the binary. SEB refuses to start an exam
# without it.
EXT="$ROOT/build/bin/webkit-extensions/libseb-web-extension.so"
[[ -f "$EXT" ]] || { echo "error: missing $EXT" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")/webkit-extensions"
cp "$EXT" "$(dirname "$OUT")/webkit-extensions/"
echo "wrote $(dirname "$OUT")/webkit-extensions/libseb-web-extension.so"
