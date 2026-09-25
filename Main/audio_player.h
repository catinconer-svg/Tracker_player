#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "driver/i2s_std.h"  // ★ ДОБАВЛЯЕМ ДЛЯ i2s_chan_handle_t ★

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool player_stopped_for_explorer;
extern char g_selected_sf2_path[256];

// ★ ДЛЯ ЗВУКА GAMEBOY ★
extern i2s_chan_handle_t tx_chan;

typedef enum {
    REPEAT_ALL = 0,
    REPEAT_ONE,
    REPEAT_SHUFFLE
} repeat_mode_t;

typedef enum {
    PLAYER_CMD_SET_VOLUME,
    PLAYER_CMD_NEXT_TRACK,
    PLAYER_CMD_PREV_TRACK,
    PLAYER_CMD_TOGGLE_REPEAT,
    PLAYER_CMD_RELOAD_SF2,
} player_cmd_type_t;

typedef struct {
    player_cmd_type_t type;
    int value;
} player_cmd_t;

extern QueueHandle_t player_cmd_queue;

typedef enum {
    EQ_FLAT = 0,
    EQ_BASS_BOOST,
    EQ_ROCK,
    EQ_POP,
    EQ_JAZZ,
    EQ_CLASSICAL,
    EQ_TALK
} eq_preset_t;

typedef enum {
    PLAYER_MODE_NONE = 0,
    PLAYER_MODE_MP3,
    PLAYER_MODE_FLAC,
    PLAYER_MODE_WAV,
    PLAYER_MODE_OPUS,
    PLAYER_MODE_XMP,
    PLAYER_MODE_GME,
    PLAYER_MODE_MIDI,
    PLAYER_MODE_USB_MIDI,
    PLAYER_MODE_GB,
} player_mode_t;

esp_err_t audio_player_init(void);
esp_err_t audio_player_play(const char *filepath);
esp_err_t audio_player_play_gb_async(const char *filepath);
void audio_player_stop(void);
void audio_player_process_ui(void);
void audio_player_toggle_pause(void);
void audio_player_toggle_mute(void);
void audio_player_volume_up(void);
void audio_player_volume_down(void);
void audio_player_prev_track(void);
void audio_player_next_track(void);
void audio_player_toggle_repeat(void);
void scan_playlist(const char *track_path);
bool is_supported_ext(const char *ext);
void audio_player_clear_ui_queue(void);
void audio_player_set_eq(eq_preset_t preset);
eq_preset_t audio_player_get_eq(void);
void audio_player_start_usb_midi(const char *sf2_path);
player_mode_t audio_player_get_current_mode(void);
int audio_player_get_volume(void);

extern char g_midi_player_sf2_path[256];
extern char g_usb_synth_sf2_path[256];

/* Получить пути к папкам */
const char* audio_player_get_midi_sf2_folder(void);
const char* audio_player_get_usb_synth_sf2_folder(void);

/* Получить/установить пути к SF2 */
const char* audio_player_get_midi_sf2_path(void);
const char* audio_player_get_usb_synth_sf2_path(void);
void audio_player_set_midi_sf2_path(const char *path);
void audio_player_set_usb_synth_sf2_path(const char *path);

/* GME настройки */
typedef struct {
    double treble;
    double bass;
    double stereo_depth;
    bool accuracy;
    bool mute_voices[8];
} gme_settings_t;

gme_settings_t audio_player_get_gme_settings(void);
void audio_player_set_gme_settings(const gme_settings_t *settings);
void audio_player_apply_gme_settings(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PLAYER_H