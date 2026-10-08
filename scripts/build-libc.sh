#!/bin/sh
# Reproduce the clean-room libc.prx ShadowMountPlus expects in sce_module/.
# GPL-3.0-or-later, from ProsperoStore commit 8c38b615. Not a Sony library.
# Logs go to stderr. The verified file path is the only stdout line.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
expected=e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036

matches() {
    file=$1
    actual=$(sha256sum "$file" | awk '{print $1}')
    [ "$actual" = "$expected" ]
}

if [ -f "$root/sce_module/libc.prx" ] && matches "$root/sce_module/libc.prx"; then
    printf '%s\n' "$root/sce_module/libc.prx"
    exit 0
fi

out="$root/build/host/libc.prx"
if [ -f "$out" ] && matches "$out"; then
    printf '%s\n' "$out"
    exit 0
fi

tool=$(sh "$root/scripts/build-self-tool.sh")
builder=$(dirname "$tool")/libc-builder
test -x "$builder"
raw="$root/build/host/libc.raw.elf"
mkdir -p "$root/build/host"
"$builder" \
    "$root/third_party/ps5-native/runtime/api-surface.txt" \
    "$root/third_party/ps5-native/runtime/imports.txt" \
    "$raw" >&2
"$tool" self --sign --in "$raw" --out "$out" >&2
if ! matches "$out"; then
    echo "libc.prx hash mismatch" >&2
    exit 1
fi
printf '%s\n' "$out"
