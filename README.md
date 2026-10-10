# YouTube Music for PS5

A native YouTube Music client for a jailbroken PS5 on firmware 13.60, played with the DualSense: search, Home recommendations, song mixes, and your liked songs. It does not include a jailbreak.

This is a fan project, not affiliated with Google, YouTube, or Sony.

## Install

Download the latest [release](https://github.com/allnewryan1/ytm_ps5/releases). It has two builds of the same code:

- **`ytmusic-*.elf`**: send it to an ELF loader listening on port 9021.
- **`PPSA99105.zip`**: a home-screen app for ShadowMountPlus. Unzip it and copy the `PPSA99105` folder (not the zip) to `/data/homebrew/`. If the tile does not appear, set the folder and its contents to mode `777` from your FTP client.

Either way, the app starts its own background player through the ELF loader on port 9021 (etaHEN or elfldr) and stops it when you quit or close the app.

## Controls

| Input | Action |
| --- | --- |
| D-pad / left stick | Move |
| Cross | Open, play, or pause |
| Square | Song menu: Play, Play next, Add to queue, Show album, Show artist. On Search, deletes a character |
| Circle | Back |
| Triangle | Now playing and the queue |
| Options | Play or pause |
| Touchpad | Repeat: off, all, one |
| L1 / R1 | Previous / next |
| L2 / R2 | Volume, on Now playing |
| Left / right on Now playing | Seek 10 seconds |

Playing a song starts a mix built from it. If the song is already in the queue, playback jumps to it instead. Add to queue and Play next add only that song. Opening an album or playlist plays that list.

Music videos are marked **Video** and podcast episodes **Episode**. A USB keyboard also works: arrows, Enter, Esc, and Backspace.

## Your own Home recommendations

Signing in with the on-screen code loads your library. For Home to show your own recommendations, YouTube Music needs a token from your own Google OAuth client, the same requirement [ytmusicapi](https://ytmusicapi.readthedocs.io/en/stable/setup/oauth.html) has. Without one, Home shows public picks.

1. In the [Google Cloud Console](https://console.cloud.google.com/), create a project and enable the **YouTube Data API v3**.
2. Under **APIs & Services → Credentials**, create an **OAuth client ID** of type **TVs and Limited Input devices**. If the consent screen is in testing, add your Google account as a test user.
3. Save the ID and secret on the console as `/data/ytmusic/oauth_client.txt`:

   ```
   client_id=1234567890-abc.apps.googleusercontent.com
   client_secret=GOCSPX-...
   ```

4. Restart the app, then sign out and back in from **Account**. The Account page shows which sign-in is in use.

## If a song will not play

The status line shows YouTube's error. Songs that cannot be opened are skipped when the queue moves on.

## Build

You need the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) v0.42 at `/opt/ps5-payload-sdk`, with pacbrew SDL2 and FFmpeg 7 (pacbrew-repo v0.39) installed into its `target/user/homebrew` prefix. The `Dockerfile` sets all of that up.

```bash
make                     # ytmusic.elf
PS5_HOST=192.168.1.50 make test    # send it to the loader on port 9021

# Home-screen folder: decode and encode the launcher images first (needs Pillow and NumPy)
sh scripts/decode-assets.sh
python3 scripts/encode-bc7.py sce_sys/background-source.jpg sce_sys/pic0.dds
python3 scripts/encode-bc7.py sce_sys/launch-background-source.jpg sce_sys/pic1.dds
make package             # dist/PPSA99105/
```

With Docker: `docker build -t ytm-ps5 . && docker run --rm -v "$PWD:/workspace" ytm-ps5 make`.

Pushes to `main` and pull requests are built and checked by [`build.yml`](.github/workflows/build.yml). A green build on `main` is published by [`release.yml`](.github/workflows/release.yml) as `v<VERSION>-<commit>`.

## Notices

Home, charts, liked songs, and mixes follow the requests [ytmusicapi](https://github.com/sigma67/ytmusicapi) makes (MIT; see [NOTICE](NOTICE)). The library itself is not included. `sce_module/libc.prx` is a clean-room runtime shim from [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore) (GPL-3.0-or-later), not a Sony library.
