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
- The picture is drawn on the window framebuffer. This SDL port has no render driver.
- Pressing X on a shelf starts that shelf. Audio is a direct AAC stream from the visionOS player client.
- If playback cannot start, the reason is shown as a notification as well as on screen.
- The interface uses a plain sans-serif face, and the bottom edge lists the controller buttons.
- Account sign-in uses YouTube's TV device code. Open google.com/device, enter the code, and liked songs load for that account. The sign-in is stored on the console.
- Options opens search. The touchpad cycles repeat.
- While the player has focus, a gold outline and a "Controlling the player" label show that playback is what the pad is driving. The side rail names the current section.
