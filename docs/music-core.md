# Background audio: the music core

Found by reading Spotify's PS5 app (PPSA05688, version 01.020.000), decrypted from a console.

## What Spotify does

- The app itself is a web app (`applicationCategoryType` 66048, Web Based Media App, with a `webAppUri`). Its audio engine is a separate native ELF at `app0/eboot2.bin`, built from `CustomMusicCore.elf`.
- `param.json` names that engine: `"musicCoreName": "CustomMusicCore"`, `"musicCoreTitleId": "NPXS40201"`. This second process is what keeps playing during games. Ordinary app audio is muted in the background, whatever port or user it opens; that matches every probe we ran.
- `eboot2.bin` imports only these modules:
  - `libSceMusicCoreInterface`
  - `libSceSysmodule`
  - `libSceCustomMusicSysCallWrapper`
  - `libSceCustomMusicAudioOut`
  - `libc` (the app's `sce_module/libc.prx`)
  - `libkernel` (four functions only)
- Networking, threads and sleeping all go through `sceCustomMusic*` wrappers, for example `sceCustomMusicNetSocket`, `sceCustomMusicPthreadCreate` and `sceCustomMusicKernelNanosleep`.

## Startup, as disassembled

```
main(argc, argv)
  sceSysmoduleLoadModule(0x123)   MUSIC_CORE_INTERFACE
  sceSysmoduleLoadModule(0x121)   CUSTOM_MUSIC_SYS_CALL_WRAPPER
  sceSysmoduleLoadModule(0x122)   CUSTOM_MUSIC_AUDIO_OUT
  sceSysmoduleLoadModule(0x103)   SRC_UTILITY (resampler)
  sceMusicCoreIfInitializeInterface(argv[1], &(uint64_t[2]){0x10, 0x12})
  sceMusicCoreIfSetFunctionTable(table)   48 function pointers, some NULL
  sceMusicCoreIfMainLoop()                blocks; the system calls the table
```

Audio:
- Setup is `sceCustomMusicAudioOutInitialize(1024, 1, 1)`.
- Playback is `sceCustomMusicAudioOutOutput(buf)`, where each buffer is 4096 bytes: 1024 frames of 48 kHz S16 stereo.
- Spotify resamples 44.1 kHz to 48 kHz with `sceSrcUtility*`.
- `Output(NULL)` drains.

Callback slots identified so far (Spotify's handler names):

| Slot | What it does |
| --- | --- |
| 0 | Status out: 0x20 bytes, then two 1-based ints |
| 1 | `initializeCustomMusicCore`: creates the engine |
| 2 | Tear-down |
| 3 | `addSrc`: a track source with `trackuri` / `trackuid` / `trackind` |
| 5, 6 | Call a completion callback right away |
| 29 | Fills a 0x3020-byte state block |
| 41 | Fills a 0x2530-byte metadata block made of 0x155-byte strings (title, artist, …) |

Slots 24, 25, 26, 32 and 42 return constants, and others return a bool, int or double. `src/musiccore.c` copies these behaviours.

The web UI controls the core through Spotify Connect (a Zeroconf HTTP server inside the core, plus Spotify's cloud), so app-to-core control is a socket.

## Status

`src/musiccore.c` is a test core with this exact shape. It logs every callback as `ytmcore:` in the kernel log and plays a pulsed tone when the system starts it. Still unknown:
- Whether the system starts a core for a native media app (category 65536).
- What the remaining slots mean.
