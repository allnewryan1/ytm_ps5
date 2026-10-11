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

## How the system starts it (Spotify kernel log)

About a second after Spotify's app shows its first frame, ShellCore does:

```
[PSM] Got handle=0x3 from AppId=0xa018
[SceLncService] launchApp(NPXS40201)
[SceLncService] appType={SCE_LNC_APP_TYPE_DAEMON} contentVersion={01.020.000} appCategoryType={0x02000100}
  ... mounts the app's own package as app0, sandboxType 2 ...
[MusicPlayerService][INFO] setCustomMusicCoreStatus: changed CustomMusicCoreStatus INACTIVE -> ACTIVE
EXEC /app0/eboot2.bin [user] ... abi=native category=custom_music_core
```

From then on, the process `NPXS40201 CustomMusicCore` stays up while Spotify is suspended in a game.

Our native media app with the same `musicCore*` lines gets "MusicPlayerService: MediaApp launch start." but no `launchApp(NPXS40201)`. Asking for the BGM CPU budget (`sceSystemServiceAcquireBgmCpuBudget(0)`, which returns 0) doesn't change that.

## Tests so far (PPSA99106, declared as a Web Based Media App)

- The system web-app launcher runs it, with the same process set Spotify gets: SceWebAppLauncher, NKUIProcess, NKNetworkProcessMediaApp, NKWebProcessMediaApp and SlimGLServerProcess. The page loads (`[PSM] Got handle`, `CanvasView first render`), but the core is still never launched.
- `app.db` (`/system_data/priv/mms/app.db`, table `tbl_contentinfo`, column `AppInfoJson`) stores `MUSIC_CORE_NAME: CustomMusicCore` for both Spotify and our folder app, so ShadowMountPlus keeps the field.
- Differences that remain:
  - `_contents_location`: 0 for Spotify (a real PFS package, `/user/app/PPSA05688/app.pkg`), 2 for our homebrew folder.
  - The web page: Spotify's TV app versus a placeholder.
  - In Spotify's log, the launcher makes a second AppDb query right before the core launch. Ours never makes it.
- The system keeps a data folder for the core at `/system_data/music_core/NPXS40201/`. It was empty here.
- Next: decrypt and read SceWebAppLauncher (`/web_app_launcher/eboot.bin`) to find what it checks before `launchApp(NPXS40201)`.

## Caution: NPXS40201 is shared

`musicCoreTitleId` NPXS40201 is the system's music core slot, and Spotify uses it. Our test packages declared the same ID. After they were installed, Spotify stopped launching on the tester's console. Don't install test packages that claim NPXS40201 next to Spotify. Probe with Spotify removed, or find out how the slot is assigned first. From now on this project declares its core as `musicCoreTitleId` **NPXS40205**, in `meta/param.json` and `scripts/make-coretest.sh`. On the tester's console (FW 13.60), NPXS40205 isn't in use. Whether any other firmware or console uses it isn't known; there's no complete public list of title IDs to check against.

## Status

`src/musiccore.c` is a test core with this exact shape. It logs every callback as `ytmcore:` in the kernel log and plays a pulsed tone when the system starts it. Still unknown:
- What makes the launcher start a core. It starts one for Spotify, but not for a native media app or a folder-installed web media app.
- What the remaining slots mean.
