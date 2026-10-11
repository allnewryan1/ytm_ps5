#!/bin/sh
# Test package PPSA99106: the same app and music core as dist/PPSA99105, declared as a
# Web Based Media App (category 66048) like Spotify, so the system web-app launcher runs it and
# should start eboot2.bin as its music core. Run after `make package`.
#
# WARNING: musicCoreTitleId NPXS40201 is the system music core slot Spotify (PPSA05688) also
# uses. Installing this package can break Spotify's background player until this package is
# removed. Only run it with that understood: pass --i-understand-it-claims-npxs40201.
set -eu
if [ "${1:-}" != "--i-understand-it-claims-npxs40201" ]; then
    echo "refusing: this package claims Spotify's music core slot (see the header)" >&2
    exit 2
fi
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
d.update(musicCoreName="CustomMusicCore", musicCoreTitleId="NPXS40201", titleId="PPSA99106", contentId="UP9000-PPSA99106_00-YTMCORETEST00001", conceptId="99106",
         applicationCategoryType=66048, webAppUri="https://example.com/",
         sdkVersion="0x1300000000000000", requiredSystemSoftwareVersion="0x1360000000000000")
d["localizedParameters"]["en-US"]["titleName"] = "YTM core test"
d.pop("gameIntent", None)
json.dump(d, open(p, "w"), indent=2)
PY
printf 'powerControlSettings=true\n' > "$out/weblauncher.conf"
sed -i 's/YouTube Music"/YTM core test"/g' "$out/homebrew.js"
printf '%s\n' "$out"
