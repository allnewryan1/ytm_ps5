#!/bin/sh
# Homebrew folder layout used by the PS5 ELF loader / websrv item list.
# Copy dist/YouTubeMusic to /data/homebrew/ on a console that already
# runs a homebrew mounter (the same place EVO Player's package script targets).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test -f "$root/ytmusic.elf"
rm -rf "$root/dist/YouTubeMusic"
mkdir -p "$root/dist/YouTubeMusic/sce_sys"
cp "$root/ytmusic.elf" "$root/dist/YouTubeMusic/eboot.elf"
cp "$root/meta/homebrew.js" "$root/dist/YouTubeMusic/homebrew.js"
cp "$root/meta/param.json" "$root/dist/YouTubeMusic/sce_sys/param.json"
echo "$root/dist/YouTubeMusic"
