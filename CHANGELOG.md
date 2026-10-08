# Changelog

## 0.1.0

- First payload: YouTube Music search, shelves, playback, and a library file on the console.
- SDL2 picture, FFmpeg audio, sceHttp2 for Innertube.
- ShadowMountPlus folder: `eboot.bin`, `sce_module/libc.prx`, and `sce_sys/`.
- Home-screen `eboot.bin` is a native PS5 module, not the elfldr payload.
- That module imports `dlopen` from libkernel. The payload libc hook (`__dlopen`) is not linked, so the loader can accept the file.
- Kernel calls such as the startup notification go through `libkernel`, which a game process loads. `libkernel_sys` is not mapped, and those imports were null.
- `libSceKeyboard` and `libSceImeDialog` are not mapped into a game either. SDL calls them through libkernel after those sprx are loaded, instead of a null import.
