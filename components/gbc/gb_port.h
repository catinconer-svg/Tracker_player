#ifndef GB_PORT_H
#define GB_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  СТРУКТУРА ХОСТА
// ============================================================================

typedef struct {
    struct {
        bool enabled;
        int format;
        int colorize;
        uint16_t *buffer16;
        uint8_t *buffer8;
        uint16_t palette[64];
        void (*blit_func)(void);   // ← Указатель на функцию
    } video;

    struct {
        bool enabled;
        bool stereo;
        int samplerate;
        int16_t *buffer;
        size_t pos;
        size_t len;
    } audio;

    int pad;
} gb_host_port_t;

// ============================================================================
//  ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================================

extern gb_host_port_t gb_host;
extern bool gb_sram_dirty;

// ============================================================================
//  ФУНКЦИИ ПОРТА
// ============================================================================

int gb_port_init(int samplerate, bool stereo, int pixformat, void *blit_func);
void *gb_port_get_framebuffer(void);
uint16_t *gb_port_get_palette(void);
void gb_port_submit_audio(int16_t *buffer, size_t samples);
int gb_port_get_pad(void);
void gb_port_set_pad(int pad);
bool gb_port_is_sram_dirty(void);
int gb_port_load_sram(const char *path);
int gb_port_save_sram(const char *path, bool quick);
int gb_port_load_state(const char *path);
int gb_port_save_state(const char *path);

#ifdef __cplusplus
}
#endif

#endif // GB_PORT_H