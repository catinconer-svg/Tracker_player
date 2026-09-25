#ifndef MIDI_PLAYER_H
#define MIDI_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration для tml_message */
struct tml_message;

#ifdef __cplusplus
extern "C" {
#endif

/* Информация о MIDI файле */
typedef struct {
    int used_channels;
    int used_programs;
    int total_notes;
    unsigned int time_length_ms;
    unsigned int time_first_note_ms;
} midi_info_t;

/* Информация об использованных программах */
typedef struct {
    uint16_t bank;
    uint16_t program;
    int channel;
} midi_program_usage_t;

/* Глобальные переменные для доступа из других файлов */
extern class Synth* g_synth;
extern class SF2Parser* g_parser;
extern struct tml_message* g_midi_data;
extern bool g_is_playing;
extern unsigned int g_total_time_ms;
extern unsigned int g_current_time_ms;
/* Функции */
bool midi_player_init(const char *sf2_path);
void midi_player_play(const char *midi_path);
void midi_player_stop(void);
void midi_player_render(float *outL, float *outR, int samples);
bool midi_player_is_playing(void);
bool midi_player_is_paused(void);
void midi_player_set_volume(float volume);
void midi_player_pause(bool pause);
unsigned int midi_player_get_total_time_ms(void);
unsigned int midi_player_get_position_ms(void);
bool midi_player_get_info(const char *midi_path, midi_info_t *info);
void midi_player_deinit(void);
bool midi_player_reload_for_midi(const char *midi_path, const char *sf2_path);
/* Новая функция для анализа использованных программ */
int midi_get_program_usage(const char *midi_path, midi_program_usage_t *usage, int max_count);

#ifdef __cplusplus
}
#endif

#endif // MIDI_PLAYER_H