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

Cross on a song plays that song. If it is already in the queue, playback jumps to it. Otherwise the queue becomes that song followed by the automix YouTube Music builds from it, the same list ytmusicapi `get_watch_playlist` returns. Signed in with your own OAuth client (below), that mix also follows your account's taste. When the mix runs out with repeat off, it continues from the last song. Square is where Play next and Add to queue live; those add only the one song and never replace the queue. Opening an album or a playlist with Cross still replaces the queue with that list.

Rows marked **Video** are music videos (YouTube Music `MUSIC_VIDEO_TYPE_OMV`, `UGC`, or `OFFICIAL_SOURCE_MUSIC`). Unmarked rows are songs (`ATV`). Podcast episodes are marked **Episode**.

A USB keyboard works too: arrows, Enter, Esc, and Backspace.

## Your own Home recommendations

Since November 2024, YouTube Music answers HTTP 400 to a token from the YouTube TV sign-in that the console uses by default. ytmusicapi has the same rule: its OAuth needs your own Google Cloud OAuth client. Without one, signing in still loads Library, and Home shows public picks for your country.

To get your own Home:

1. In the [Google Cloud Console](https://console.cloud.google.com/), create a project and enable the **YouTube Data API v3**.
2. Under **APIs & Services → Credentials**, create an **OAuth client ID** of type **TVs and Limited Input devices**. If the consent screen is in testing, add your Google account as a test user.
3. Put the client ID and secret on the console in `/data/ytmusic/oauth_client.txt`, one per line:

   ```
   client_id=1234567890-abc.apps.googleusercontent.com
   client_secret=GOCSPX-...
   ```

4. Restart the app. In **Account**, sign out and sign in again with the new code.

The Account page says which sign-in is in use. `/data/ytmusic/auth.txt` keeps the refresh token and which client it came from.

## If a song will not play

YouTube sometimes returns a signed stream instead of a direct audio URL, or it refuses the player client. The status line on screen is the actual error. Search still works when the catalog responds. Playback needs a `googlevideo.com` audio URL and an FFmpeg build with HTTPS.

## Notices

Home, charts, liked songs, and the automix follow the requests used by [ytmusicapi](https://github.com/sigma67/ytmusicapi) (`get_home`, `get_charts`, `get_liked_songs`, and `get_watch_playlist`), including its OAuth client flow. That project is MIT licensed. The notice is in [NOTICE](NOTICE). This program does not include the Python library.
