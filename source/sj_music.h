/* sj_music.h -- soundtrack playback for the fake musicplayer class.
 * MIT licensed, see LICENSE. */
#ifndef SJ_MUSIC_H
#define SJ_MUSIC_H
void sj_music_init(const char *asset_dir);
void sj_music_play(const char *name, int loop);
void sj_music_stop(void);
void sj_music_pause(void);
void sj_music_resume(void);
void sj_music_set_volume(float v);
int  sj_music_is_playing(void);
void sj_music_exit(void);
#endif
