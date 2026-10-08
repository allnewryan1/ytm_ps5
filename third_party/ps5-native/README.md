Host tools that package the ShadowMountPlus folder.

`ps5-native-tool link` turns the address-0 PIE into a PS5 module
(`e_type` `0xFE10`). `self --sign` then wraps that module as `eboot.bin`
(development FSELF magic `0x1D3D154F`). `libc-builder` reproduces
`sce_module/libc.prx` from `runtime/api-surface.txt` and
`runtime/imports.txt`. `app_crt.cpp`, `ps5-pie.ld`, and `app-symbols.map`
are the startup and link layout for that module. `payload_compat.c` is local:
libc's `dlfcn.o` and `mman.o` are left out of the home-screen link so
`dlopen` and `mmap` stay libkernel imports. The payload libc's weak
`__dlopen` hook is what made the module converter reject the PIE.

The sources are GPL-3.0-or-later, from
[ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore)
commit `8c38b6159595717a8b642569ed6fc6f4593ab5b3` (`tooling/native`).
`sce_module_writer.cpp` lists every missing stub import in one error.
`payload_compat.c` is not from that tree.
The license is [LICENSE](LICENSE). `libc.prx` is that project's clean-room
runtime shim, not a Sony library. Its SHA-256 is
`e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
