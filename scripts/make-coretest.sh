#!/bin/sh
# Test package PPSA99106: the same app and music core as dist/PPSA99105, declared as a
# Web Based Media App (category 66048) like Spotify, so the system web-app launcher runs it and
# should start eboot2.bin as its music core. Run after `make package`.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
src="$root/dist/PPSA99105"
out="$root/dist/PPSA99106"
test -f "$src/eboot2.bin"
rm -rf "$out"
cp -r "$src" "$out"
python3 - "$out/sce_sys/param.json" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d.update(titleId="PPSA99106", contentId="UP9000-PPSA99106_00-YTMCORETEST00001", conceptId="99106",
         applicationCategoryType=66048, webAppUri="https://example.com/",
         sdkVersion="0x1300000000000000", requiredSystemSoftwareVersion="0x1360000000000000")
d["localizedParameters"]["en-US"]["titleName"] = "YTM core test"
d.pop("gameIntent", None)
json.dump(d, open(p, "w"), indent=2)
PY
printf 'powerControlSettings=true\n' > "$out/weblauncher.conf"
sed -i 's/YouTube Music"/YTM core test"/g' "$out/homebrew.js"
printf '%s\n' "$out"
