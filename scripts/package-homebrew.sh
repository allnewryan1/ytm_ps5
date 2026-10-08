#!/bin/sh
# Folder ShadowMountPlus scans under /data/homebrew/PPSA99105/:
#   eboot.bin              development FSELF (magic 0x1D3D154F)
#   sce_module/libc.prx    clean-room runtime shim
#   sce_sys/               param.json, icon0.png, pic0.dds, pic1.dds
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
test -f "$root/ytmusic.elf"
for need in \
    "$root/meta/homebrew.js" \
    "$root/meta/param.json" \
    "$root/sce_sys/icon0.png" \
    "$root/sce_sys/pic0.dds" \
    "$root/sce_sys/pic1.dds"
do
    test -f "$need"
done

tool=$(sh "$root/scripts/build-self-tool.sh")
libc=$(sh "$root/scripts/build-libc.sh")
test -x "$tool"
test -s "$libc"

app="$root/dist/PPSA99105"
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$root/ytmusic.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$libc" "$app/sce_module/libc.prx"
cp "$root/meta/homebrew.js" "$app/homebrew.js"
cp "$root/meta/param.json" "$app/sce_sys/param.json"
cp "$root/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$root/sce_sys/pic0.dds" "$app/sce_sys/pic0.dds"
cp "$root/sce_sys/pic1.dds" "$app/sce_sys/pic1.dds"

magic=$(od -An -t x1 -N 4 "$app/eboot.bin" | tr -d ' \n')
test "$magic" = "4f153d1d"
prx=$(od -An -t x1 -N 4 "$app/sce_module/libc.prx" | tr -d ' \n')
test "$prx" = "5414f5ee"
test ! -e "$app/eboot.elf"
printf '%s\n' "$app"
