#ifndef GB_DISPLAY_ADAPTER_H
#define GB_DISPLAY_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool gb_display_adapter_init(lv_obj_t *parent);
void gb_display_adapter_update_rgb(uint16_t *rgb_buffer);
void gb_display_adapter_deinit(void);
bool gb_display_adapter_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif // GB_DISPLAY_ADAPTER_H