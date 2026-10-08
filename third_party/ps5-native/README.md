Host tools that package the ShadowMountPlus folder.

`ps5-native-tool self --sign` wraps `ytmusic.elf` as `eboot.bin` (development
FSELF magic `0x1D3D154F`). `libc-builder` reproduces `sce_module/libc.prx`
from `runtime/api-surface.txt` and `runtime/imports.txt`.

The sources are GPL-3.0-or-later, taken unchanged from
[ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore)
commit `8c38b6159595717a8b642569ed6fc6f4593ab5b3` (`tooling/native`).
The license is [LICENSE](LICENSE). `libc.prx` is that project's clean-room
runtime shim, not a Sony library. Its SHA-256 is
`e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
