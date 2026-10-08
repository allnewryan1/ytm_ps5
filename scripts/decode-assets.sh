#!/bin/sh
# The GitHub file API used for this repo stores the launcher art as base64.
# Decode it back to the PNG icon and the JPEG background sources.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root/sce_sys"
base64 -d icon0.png.b64 > icon0.png
base64 -d background-source.jpg.b64 > background-source.jpg
base64 -d launch-background-source.jpg.b64 > launch-background-source.jpg
