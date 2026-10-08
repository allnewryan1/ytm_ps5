# Changelog

## 0.1.0

- First payload: YouTube Music search, shelves, playback, and a library file on the console.
- SDL2 picture, FFmpeg audio, sceHttp2 for Innertube.
- ShadowMountPlus folder: `eboot.bin`, `sce_module/libc.prx`, and `sce_sys/`.
- Home-screen `eboot.bin` is a native PS5 module, not the elfldr payload.
- That module imports `dlopen` from libkernel. The payload libc hook (`__dlopen`) is not linked, so the loader can accept the file.
- Kernel calls such as the startup notification go through `libkernel`, which a game process loads. `libkernel_sys` is not mapped, and those imports were null.
- Home screen title is YouTube Music. The install folder is still `PPSA99105`.
- Icon and background art are a record and a listening room, not the old play-button tile.
- Scanout memory comes from the game direct-memory pool. Leaving the process no longer uses the exit syscall the shell reports as a crash.
