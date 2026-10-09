#ifndef YTM_PLAYER_H
#define YTM_PLAYER_H

int player_open(char *err, int err_n);
void player_close(void);

/* Stop anything currently decoding and start this URL. 0 on spawn. */
int player_start(const char *url, int duration_s);
/* Same, but the bytes stay in memory. Takes ownership of data. */
int player_start_mem(unsigned char *data, int n, int duration_s);
/* Replay the current song from the start. 0 if something was playing. */
int player_replay(void);
void player_stop(void);
void player_toggle(void);
void player_seek_by(double delta_s);
void player_volume_add(int delta);

int player_paused(void);
int player_ended(void);
void player_ack_ended(void);
double player_position(void);
double player_duration(void);
int player_volume(void);
/* Pointer is stable until the next player_* call. Empty if none. */
const char *player_error(void);

#endif
