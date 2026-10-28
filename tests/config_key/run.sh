#!/usr/bin/env bash
# Builds and runs the Config Key test. Needs Qt6 Core/Xml dev and zlib.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$here/../.."
out="${TMPDIR:-/tmp}/seb-config-key-test"
g++ -std=c++20 -O1 -fPIC -I"$root/src" "$here/main.cpp" "$root/src/settings/settings_parsing.cpp" \
    $(pkg-config --cflags --libs Qt6Core Qt6Xml zlib) -o "$out"
"$out" "$here"
