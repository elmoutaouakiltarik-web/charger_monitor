#!/bin/bash
# Proves the library is consumable by a third party: install the package,
# then build an external project that only does find_package + link.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${CHARGER_MONITOR_BUILD_DIR:-./build}"
[ -d "$BIN" ] || { echo "E2E FAIL: build dir not found: $BIN" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cmake --install "$BIN" --prefix "$work/prefix" >/dev/null

cmake -S examples/consume_library -B "$work/consumer" -DCMAKE_PREFIX_PATH="$work/prefix" >/dev/null
cmake --build "$work/consumer" --parallel >/dev/null

out="$("$work/consumer/consumer")"
echo "$out"

if ! echo "$out" | grep -q '4800 Wh'; then
    echo "E2E FAIL: downstream consumer did not compute 4800 Wh" >&2
    exit 1
fi
if ! echo "$out" | grep -q '5300000 micro-EUR'; then
    echo "E2E FAIL: downstream consumer did not compute 5.30 EUR" >&2
    exit 1
fi
if ! echo "$out" | grep -q 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'; then
    echo "E2E FAIL: sha256 does not match the published NIST vector" >&2
    exit 1
fi

echo "E2E OK: find_package consumer builds and computes 4800 Wh / 5.30 EUR exactly"