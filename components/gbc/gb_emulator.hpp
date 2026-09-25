#ifndef GB_EMULATOR_HPP
#define GB_EMULATOR_HPP

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  УПРАВЛЕНИЕ ЭМУЛЯТОРОМ
// ============================================================================

bool gb_emulator_init(const char *rom_path);
void gb_emulator_deinit(void);
void gb_emulator_run_frame(void);
bool gb_emulator_is_running(void);
void gb_emulator_reset(void);

// ============================================================================
//  УПРАВЛЕНИЕ
// ============================================================================

void gb_emulator_set_button(uint8_t button, bool pressed);
void gb_emulator_update_input(void);

// ============================================================================
//  ПОЛУЧЕНИЕ КАДРА
// ============================================================================

uint16_t* gb_emulator_get_framebuffer(void);

// ============================================================================
//  СОХРАНЕНИЕ/ЗАГРУЗКА
// ============================================================================

bool gb_emulator_load_state(const char *path);
bool gb_emulator_save_state(const char *path);

// ============================================================================
//  КОНСТАНТЫ КНОПОК — ИСПОЛЬЗУЙ ИЗ gnuboy.h
// ============================================================================

// ★ НЕ ОПРЕДЕЛЯЙ ЗДЕСЬ! Они уже есть в gnuboy.h ★
// #define GB_PAD_RIGHT   0x01   <- УДАЛИТЬ
// #define GB_PAD_LEFT    0x02   <- УДАЛИТЬ
// #define GB_PAD_UP      0x04   <- УДАЛИТЬ
// #define GB_PAD_DOWN    0x08   <- УДАЛИТЬ
// #define GB_PAD_A       0x10   <- УДАЛИТЬ
// #define GB_PAD_B       0x20   <- УДАЛИТЬ
// #define GB_PAD_SELECT  0x40   <- УДАЛИТЬ
// #define GB_PAD_START   0x80   <- УДАЛИТЬ

#ifdef __cplusplus
}
#endif

#endif // GB_EMULATOR_HPP