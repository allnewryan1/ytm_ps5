# YouTube Music for PS5

Native userland payload for a **jailbroken PS5 on system software 13.60**. It is a DualSense YouTube Music client: search, shelves, playback, seek, and a library stored on the console. It is not a website and it does not contain a jailbreak.

The build follows [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5) and the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): `prospero.mk`, an ELF, and `PS5_DEPLOY` to the ELF loader on port **9021**. Video playback in EVO goes through sceAgc because it is a film player. This app is music, so the picture is SDL2 on sceVideoOut and the sound is FFmpeg into sceAudioOut, which is the same homebrew prefix EVO's build installs (pacbrew SDL2 and FFmpeg). HTTPS to YouTube uses sceHttp2, the same calls as the SDK's `http2_get` sample.

Fan client. Not affiliated with Google, YouTube, or Sony.

## GitHub Actions

[`.github/workflows/build.yml`](.github/workflows/build.yml) follows EVO Player's CI: it builds this repo's Docker image (Ubuntu 24.04, clang 18, Payload SDK **v0.42**, pacbrew **v0.39**), runs `make` and `make package`, and rejects the ELF if it linked the host loader. A tag `v*` runs [`.github/workflows/release.yml`](.github/workflows/release.yml) and publishes the ELF plus the homebrew folder. The tag has to match [`VERSION`](VERSION).

Issue triage, PR labels, a first-interaction greeting, and stale-issue closing are the same kind of workflows EVO uses. Adding issues to a GitHub Project stays off until `ROADMAP_PROJECT_URL` and `ADD_TO_PROJECT_PAT` are set.

## What you need already

- A PS5 on 13.60 that **already** has an ELF loader listening on port 9021 (elfldr / the loader you use for other payloads). This repository does not include an exploit.
- The PS5 Payload SDK installed, usually at `/opt/ps5-payload-sdk`.
- pacbrew **SDL2** and **FFmpeg 7** installed into that SDK's `target/user/homebrew` tree. EVO Player's `docs/build/building.md` is the setup this Makefile expects.

## Build and send

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
export PS5_HOST=192.168.1.50
export PS5_PORT=9021
make
make test
```

`make test` runs the SDK's `PS5_DEPLOY` against the loader, the same way `samples/hello_world` does.

Folder install (copy to the console over the FTP server you already run):

```bash
make package
```

That writes `dist/YouTubeMusic/` (`eboot.elf`, `homebrew.js`, `sce_sys/param.json`, title id `YTMS00001`). Put that directory in `/data/homebrew/` if your mounter launches folders. On 13.60, PKG install is not the path this project uses.

## Controls

| Input | Action |
| --- | --- |
| D-pad / left stick | Move |
| Cross | Open / play / pause on the player |
| Circle | Back |
| Square | Save or remove the song in `/data/ytmusic/liked.txt` |
| Triangle | Now playing |
| Options | Search |
| Create | Repeat off / all / one |
| L1 / R1 | Previous / next |
| Left / right on the player | Seek 10 seconds |
| Up / down on the player | Volume |

A USB keyboard also works (arrows, Enter, Esc, Backspace).

## If a song will not play

YouTube sometimes returns a signed stream instead of a direct audio URL, or refuses the player client. The status line on screen is the real error. Search still works when the catalog responds. Playback needs a `googlevideo.com` audio URL and an FFmpeg build with HTTPS.
