# Changelog

## 0.9.0

- Home can show the account's own recommendations. It makes the request ytmusicapi `get_home` makes with OAuth: `music.youtube.com/youtubei/v1/browse?alt=json`, client WEB_REMIX, `"user":{}`, a Bearer token, and `X-Goog-Request-Time`. As in ytmusicapi, that needs a token from your own Google Cloud OAuth client; put it in `/data/ytmusic/oauth_client.txt` (see README), then sign out and in. Without it, Home tries the Android client from 0.8.1, then shows public picks with a note, not your liked songs.
- The song mix uses the same signed-in request as ytmusicapi `get_watch_playlist`, with the Android client from 0.8.1 as the fallback. The song starts playing first and the mix fills in behind it, and a mix that runs out continues from the last song. A song's music-video counterpart is no longer added beside it.
- Audio: clearing the buffer on a seek or a new song locks the audio device, so it cannot race the callback. Volume ramps over one buffer instead of clicking. A song ends after its last sample has played out. Dropped streams resume with a Range request at the last packet.
- Covers trim bars only when they are matched on both sides, and also trim the side bars of a 16:9 frame. A square cover with a dark edge is left alone.
- Podcast episodes are marked Episode. Video and Episode are drawn as chips.

## 0.8.1

- A new song's mix is requested from the Android Music client. Signed in, that request carries the account token, so the mix can follow the account. If Android refuses the token, the public mix for that song is used instead. The music web client is not sent the token.

## 0.8.0

- Signed-in For you no longer asks the TV client for FEmusic_home. That browse id is HTTP 400 with or without a token. Home tries the current Android Music client. If that page is empty or refused, liked songs and the library load on the TV client instead of leaving the 400 on screen.
- The search bar no longer keeps a shelf query. Rock is not prefilled as "rock hits".
- Covers prefer the square catalog image and drop the black bars above and below a letterboxed frame.
- Playing a song that is not already in the queue starts that song's radio mix. Add to queue still adds only that song.
- A song is not marked finished while decoded audio is still waiting to play, and a read that stops more than a few seconds early is tried again.
- Volume is applied as the samples play, so L2 and R2 do not wait on audio that was already queued.
- A row YouTube marks as a video, rather than an audio track, is labeled Video.

## 0.7.0

- Signed-in For you no longer calls the music web client with the device token. That call is the HTTP 400. Home now uses the TV client, the same one that loads the library, and then the Android music client if the TV page is empty. Charts are still only for signed-out home.
- Large titles are filtered instead of drawn as blocks, so they stay readable on a 1080p and a 4K screen.
- Search is a section in the side menu. On that page, Square deletes the last character. Options pauses or resumes the current song, and does nothing when nothing is playing.

## 0.6.0

- Signed-in For you uses the same call as ytmusicapi get_home. That is music.youtube.com/youtubei/v1/browse?alt=json, client WEB_REMIX, the account token, and no API key. The next page is ctoken and continuation on that same call.
- The screen follows Material 3: color roles, the shape scale, navigation pills, cards, a search bar, lists, and the player.

## 0.5.0

- Signed-in For you is the account home. The request is the music web client with that account's token, then the Android music client, then the TV client. Charts are not mixed in. Charts stay the signed-out home.
- Quick picks stay songs. An album or artist link inside the subtitle is not treated as the row. Square can open the album or the artist without playing it.
- Row covers are cached at the size the rows use, in a cache large enough to keep the home page. Coming back to home does not fetch them again. The Now Playing photo is stored apart from those rows.
- Choosing a song that is not already in the queue clears the queue and plays that song. Square opens Play, Play next, Add to queue, Show album, and Show artist.

## 0.4.0

- Songs stream. The console does not download the file first, so the screen is not stuck while the track copies. The stream address is followed to the host that serves the audio, and the file itself is left for the player.
- For you reads the YouTube Music home browse. Songs land under Quick play and playlists under Playlists. Podcasts and artist rows are skipped.
- Signed out, the country comes from the console's IP. Charts for that country supply the songs and the playlists.

## 0.3.0

- Songs download as a file and then play, so a redirect from the audio host is followed before the decoder opens the stream. If the file is too large, playback falls back to the stream URL.
- Covers prefer a 720-wide photo and are kept at 360 pixels, instead of a small thumbnail blown up on screen.
- Triangle opens Now Playing. The queue is the list on the right of that card. Up and down move through it, X plays the highlighted song, and L2 and R2 change the volume.
- Repeat is a light disc with a dark loop: arrows for repeat all, a 1 for repeat one, and a slash for off.
- For you is two columns. Quick play is individual songs. Playlists are the mixes, with the artist line shortened so both columns fit.

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
