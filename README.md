# YouTube Music for PS5

Native userland payload for a jailbroken PS5 on system software 13.60. It is a DualSense client for searching, browsing shelves, playing songs, and opening the liked songs on a signed-in account. It is not a website, and it does not include a jailbreak.

The build uses the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk): `prospero.mk`, an ELF, and `PS5_DEPLOY` to an ELF loader on port **9021**. Video is SDL2 on sceVideoOut. Audio is FFmpeg into sceAudioOut. HTTPS calls to YouTube use sceHttp2, the same calls as the SDK `http2_get` sample.

This is a fan client. It is not affiliated with Google, YouTube, or Sony.

## GitHub Actions

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds this repository's Docker image (Ubuntu 24.04, clang 18, Payload SDK **v0.42**, pacbrew **v0.39**), runs `make` and `make package`, and rejects the ELF if it linked the host loader. When that workflow succeeds on `main`, [`.github/workflows/release.yml`](.github/workflows/release.yml) publishes the tested ELF and homebrew folder as `v<VERSION>-<commit>`.

Other workflows label issues and pull requests, greet first-time contributors, and close stale issues and pull requests. Adding issues to a GitHub Project stays off until `ROADMAP_PROJECT_URL` and `ADD_TO_PROJECT_PAT` are set.

## What you need

- A PS5 on 13.60 that already has an ELF loader listening on port 9021. This repository does not include an exploit.
- The PS5 Payload SDK, usually installed at `/opt/ps5-payload-sdk`.
- pacbrew **SDL2** and **FFmpeg 7**, installed into that SDK's `target/user/homebrew` tree.

## Build and send

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
export PS5_HOST=192.168.1.50
export PS5_PORT=9021
make
make test
```

`make test` runs the SDK's `PS5_DEPLOY` against the loader.

To install the homebrew folder, copy it to the console with the FTP server you already run:

```bash
make package
```

That writes `dist/PPSA99105/`. ShadowMountPlus installs that folder, not a PKG. Copy the folder (not the zip) to `/data/homebrew/` so the console sees:

- `/data/homebrew/PPSA99105/eboot.bin`
- `/data/homebrew/PPSA99105/sce_module/libc.prx`
- `/data/homebrew/PPSA99105/sce_sys/` (`param.json`, `icon0.png`, `pic0.dds`, `pic1.dds`)

The install folder stays `PPSA99105`. The home screen title is YouTube Music. `eboot.bin` is a native PS5 program (not the loader payload). `libc.prx` is a clean-room runtime shim from [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore) (GPL-3.0-or-later). It is not a Sony library. If the tile does not show up, set the folder and everything in it to mode `777` from your FTP client. The separate `ytmusic.elf` file is still what an ELF loader on port 9021 runs.

## Controls

| Input | Action |
| --- | --- |
| D-pad / left stick | Move |
| Cross | Open, play, or pause |
| Square | Song menu. On Search, deletes a character |
| Circle | Back |
| Triangle | Now playing, queue on that card |
| Options | Play or pause. Does nothing if nothing is playing |
| Touchpad | Cycle repeat: off, all, one |
| L1 / R1 | Previous / next |
| L2 / R2 | Volume, while Now playing is open |
| Left / right on the player | Seek 10 seconds |
| Up / down on the player | Move through the queue |

Cross on a song plays that song. If it is not already in the queue, the queue becomes just that song. Square is where Play next and Add to queue live. Opening an album or a playlist with Cross still replaces the queue with that list.

A USB keyboard works too: arrows, Enter, Esc, and Backspace.

## If a song will not play

YouTube sometimes returns a signed stream instead of a direct audio URL, or it refuses the player client. The status line on screen is the actual error. Search still works when the catalog responds. Playback needs a `googlevideo.com` audio URL and an FFmpeg build with HTTPS.

## Notices

Home and charts requests follow the browse identifiers used by [ytmusicapi](https://github.com/sigma67/ytmusicapi) (`FEmusic_home` and `FEmusic_charts`). That project is MIT licensed. The notice is in [NOTICE](NOTICE). This program does not include the Python library.
