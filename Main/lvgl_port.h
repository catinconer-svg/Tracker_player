#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_display_t *lvgl_port_init(void);

// ★ ДЛЯ GAMEBOY: ПРЯМАЯ ОТРИСОВКА RGB565 ★
void lvgl_port_render_gb_direct_rgb(uint16_t *rgb_buffer);

// ★ ВКЛЮЧЕНИЕ/ВЫКЛЮЧЕНИЕ РЕЖИМА (опционально) ★
void lvgl_port_enable_gb_direct_mode(bool enable);

#ifdef __cplusplus
}
#endif

#endif // LVGL_PORT_H