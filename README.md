# YouTube Music for PS5

Unofficial YouTube Music client

This is a fan client. It is not affiliated with Google, YouTube, or Sony.

## Controls

| Input | Action |
| --- | --- |
| D-pad / left stick | Move |
| Cross | Open, play, or pause |
| Circle | Back |
| Square | Save or remove the song in `/data/ytmusic/liked.txt` |
| Triangle | Now playing |
| Options | Search |
| Create | Cycle repeat: off, all, one |
| L1 / R1 | Previous / next |
| Left / right on the player | Seek 10 seconds |
| Up / down on the player | Volume |

A USB keyboard works too: arrows, Enter, Esc, and Backspace.

## If a song will not play

YouTube sometimes returns a signed stream instead of a direct audio URL, or it refuses the player client. The status line on screen is the actual error. Search still works when the catalog responds. Playback needs a `googlevideo.com` audio URL and an FFmpeg build with HTTPS.
