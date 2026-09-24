#ifndef SETTINGS_UI_H
#define SETTINGS_UI_H

#include "lvgl.h"
#include "audio_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Создаём настройки для конкретного режима */
lv_obj_t *settings_ui_create(player_mode_t mode);

void settings_ui_set_player_screen(lv_obj_t *screen);
void settings_ui_reset_focus(void);
void settings_ui_handle_esc(void);
#ifdef __cplusplus
}
#endif

#endif // SETTINGS_UI_H