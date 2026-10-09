# Changelog

## 0.2.0

- Up next is a queue. Triangle opens it, X plays the highlighted row, and the bar shows how many songs remain.
- Circle returns to the section you left, including its highlight, instead of always jumping home.
- Covers stay cached and the oldest one is dropped when the cache fills, so later rows still get art. The next song's audio URL is fetched while the current one plays.
- Library asks the TV client, which accepts the device sign-in. The music web client was answering HTTP 400.
- Repeat icons are drawn larger so they stay readable on the player screen.
- A signed-in home asks for that account's recommendations. Without a sign-in, the mood shelves stay.

## 0.1.0

- First payload: YouTube Music search, shelves, and playback.
- Explore lists New releases, Charts, and Trending from YouTube Music. Options still opens the search keyboard.

- Song, artist, album, and playlist names are read from the catalog objects, including accented characters.
- Covers are downloaded for the playing song and the rows on screen.
- Home left and right stay on the shelf until the left edge, which returns to the menu.
- Library loads liked songs from the signed-in account. Account no longer has a separate Liked songs row.
- The player label says Now Playing. Repeat is a loop icon for all, one, and off.
- Library loads liked songs for the signed-in account. The console no longer stores them in a text file.
- The now-playing bar shows the song and album on the left, the seek bar in the center, and a repeat icon on the right. Album text is taken from the catalog and falls back to the artist. Volume is a slider on the player screen.
- Home shelves are short tiles. The player outline still marks when the pad is controlling playback.
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
- Playback sends a YouTube visitor id with the stream request. Without it, YouTube answers "sign in" even after the account code is accepted. A signed-in console also tries the TV player with that token.
- Shelves, rows, search, and the player use rounded corners.
