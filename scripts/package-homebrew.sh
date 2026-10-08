#!/bin/sh
# Homebrew folder layout used by the PS5 ELF loader / websrv item list.
# Copy dist/PPSA99105 to /data/homebrew/ on a console that already
# runs a homebrew mounter.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
test -f "$root/ytmusic.elf"
rm -rf "$root/dist/PPSA99105"
mkdir -p "$root/dist/PPSA99105/sce_sys"
cp "$root/ytmusic.elf" "$root/dist/PPSA99105/eboot.elf"
cp "$root/meta/homebrew.js" "$root/dist/PPSA99105/homebrew.js"
cp "$root/meta/param.json" "$root/dist/PPSA99105/sce_sys/param.json"
echo "$root/dist/PPSA99105"
