#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_MODE_FILE_EXPLORER,
    INPUT_MODE_PLAYER
} input_mode_t;

extern volatile bool open_settings_flag;
extern volatile bool exit_settings_flag;  // <-- ДОБАВИТЬ ЭТУ СТРОКУ

void input_init(void);
void input_set_mode(input_mode_t mode);
input_mode_t input_get_mode(void);
void input_set_player_screen(lv_obj_t *screen);
void input_set_in_settings(bool state);
bool input_get_in_settings(void);

#ifdef __cplusplus
}
#endif

#endif // INPUT_H