#ifndef PLAYER_UI_H
#define PLAYER_UI_H

#include "lvgl.h"
#include "audio_player.h"

#ifdef __cplusplus
extern "C" {
#endif




lv_obj_t *player_ui_create(void);
void player_ui_set_title(const char *title);
void player_ui_update(const char *title, int pattern, int total_patterns,
                      int row, uint32_t time_ms, uint32_t total_time_ms);
void player_ui_set_pause(bool paused);
void player_ui_set_repeat_mode(repeat_mode_t mode);
void player_ui_set_format(const char *format);
void player_ui_set_volume(int volume);
void player_ui_show_loading(bool show);
void player_ui_set_module_info(int channels, int patterns, int length, 
                                int instruments, int samples, 
                                int speed, int bpm);
void player_ui_set_gme_info(const char *game, const char *author, const char *system);
#ifdef __cplusplus
}
#endif

#endif