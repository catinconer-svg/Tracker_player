#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "driver/i2s_std.h"
#include "xmp.h"
#include "audio_player.h"
#include "player_ui.h"
#include "esp_heap_caps.h"
#include "micro_flac/flac_decoder.h"
#include "micro_mp3/mp3_decoder.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "gme.h"
#include "midi_player.h"
#include <math.h>
#include "synth.h" 
#include "usb_midi.h"
#include "input.h"
#include "gb_emulator.hpp"
#include "gb_display_adapter.h"
#include "esp_timer.h"


static const char *TAG = "AUDIO_PLAYER";
static void gme_player_task(void *pvParameters);  // ← ДОБАВИТЬ ЭТУ СТРОКУ
char g_selected_sf2_path[256] = "/sdcard/soundfonts/GS Sound Set.sf2";
static player_mode_t current_player_mode = PLAYER_MODE_NONE;


/* Пути к SF2 для разных режимов */
/* Пути к SF2 для разных режимов */
char g_midi_player_sf2_path[256] = "/sdcard/soundfonts/MIDI_PLAYER/GS Sound Set.sf2";
char g_usb_synth_sf2_path[256] = "/sdcard/soundfonts/USB_SYNTH/GS Sound Set.sf2";

static lv_obj_t *gb_screen = NULL;
extern lv_obj_t *main_screen;

extern SF2Parser* g_parser;
extern Synth* g_synth;
static volatile bool usb_midi_mode = false;
/* --- I2S пины --- */
#define I2S_BCLK   GPIO_NUM_1
#define I2S_WS     GPIO_NUM_2
#define I2S_DOUT   GPIO_NUM_42

/* --- DMA настройки --- */
#define DMA_DESC_NUM    8
#define DMA_FRAME_NUM   512

/* --- Кольцевой буфер --- */
#define RINGBUF_SIZE    32768

/* --- Глобальные переменные --- */
i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t audio_ringbuf = NULL;

static esp_err_t i2s_reconfigure(uint32_t sample_rate);

static TaskHandle_t player_task_handle = NULL;
static TaskHandle_t audio_out_task_handle = NULL;
static volatile bool is_playing = false;
static volatile bool is_paused = false;
static bool stop_no_auto = false;
/* Громкость */
static volatile int current_volume = 80;
static volatile int saved_volume = 80;
static volatile bool is_muted = false;
static eq_preset_t current_eq = EQ_FLAT;

/* Очереди */
QueueHandle_t ui_update_queue = NULL;
QueueHandle_t player_cmd_queue = NULL;
/* Сигнал для main.c */
volatile bool player_stopped_for_explorer = false;

/* Плейлист */
static char playlist[32][256];
static int playlist_count = 0;
static int current_track_index = 0;

/* Режим повтора */
static repeat_mode_t repeat_mode = REPEAT_ALL;

/* GME настройки по умолчанию */
static gme_settings_t gme_settings = {
    .treble = 0.0,       // Нормальный тембр
    .bass = 15.0,        // Нормальные басы (15 Hz)
    .stereo_depth = 0.3, // Умеренная стерео глубина
    .accuracy = false,   // Точная эмуляция выключена (экономит CPU)
    .mute_voices = {false, false, false, false, false, false, false, false}
};

/* Указатель на текущий GME эмулятор (если играет GME) */
static Music_Emu* current_gme_emu = NULL;







typedef struct {
    char title[64];
    char format[32];
    int pattern;
    int total_patterns;
    int row;
    uint32_t time_ms;
    uint32_t total_time_ms;
    int mode;
    int volume;
    int vu_total_channels;
    uint8_t vu_volumes[32];
    int vu_channels;
    bool update_mode;
    bool update_title;
    bool update_format;
    bool update_volume;
    bool update_vu;
    bool update_vu_layout;
    bool show_loading;
    bool hide_loading;
    bool update_module_info;
    int mod_channels;
    int mod_patterns;
    int mod_length;
    int mod_instruments;
    int mod_samples;
    int mod_speed;
    int mod_bpm;
    bool update_gme_info;
    char gme_game[64];
    char gme_author[64];
    char gme_system[32];
} ui_update_t;

/* --- Колбеки libxmp --- */
typedef struct {
    FILE *file;
    int should_free;
} esp_xmp_file_t;

static unsigned long cb_read(void *dest, unsigned long len, unsigned long nmemb, void *priv)
{
    return fread(dest, len, nmemb, ((esp_xmp_file_t *)priv)->file);
}

static int cb_seek(void *priv, long offset, int whence)
{
    return fseek(((esp_xmp_file_t *)priv)->file, offset, whence);
}

static long cb_tell(void *priv)
{
    return ftell(((esp_xmp_file_t *)priv)->file);
}

static int cb_close(void *priv)
{
    esp_xmp_file_t *f_priv = (esp_xmp_file_t *)priv;
    int ret = 0;
    if (f_priv->file) {
        ret = fclose(f_priv->file);
        f_priv->file = NULL;
    }
    if (f_priv->should_free) {
        f_priv->should_free = 0;
        heap_caps_free(f_priv);
    }
    return ret;
}

/* --- I2S инициализация --- */
static esp_err_t i2s_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S...");

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear = true,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "I2S channel failed"); return ret; }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) { i2s_del_channel(tx_chan); tx_chan = NULL; return ret; }

    ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) { i2s_del_channel(tx_chan); tx_chan = NULL; return ret; }

    audio_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_ringbuf) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "I2S initialized");
    return ESP_OK;
}


/* Перенастройка I2S на другую частоту */
/* Перенастройка I2S на другую частоту */
static esp_err_t i2s_reconfigure(uint32_t sample_rate)
{
    if (!tx_chan) {
        ESP_LOGE(TAG, "I2S channel not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Reconfiguring I2S to %d Hz", sample_rate);
    
    /* Останавливаем и удаляем существующий канал */
    esp_err_t ret = i2s_channel_disable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable I2S: %s", esp_err_to_name(ret));
    }
    
    ret = i2s_del_channel(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(ret));
    }
    tx_chan = NULL;
    
    /* Создаём новый канал */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear = true,
    };
    
    ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create new I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Настраиваем с новой частотой */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Запускаем канал */
    ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2S reconfigured to %d Hz", sample_rate);
    return ESP_OK;
}



/* Восстановление I2S в нейтральное состояние (44.1 кГц, стерео, 16 бит) */
static void i2s_reset_to_default(void)
{
    ESP_LOGI(TAG, "Resetting I2S to default state (44.1 kHz)");
    
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    
    /* Пересоздаём audio_ringbuf если нужно */
    if (audio_ringbuf) {
        vRingbufferDelete(audio_ringbuf);
        audio_ringbuf = NULL;
    }
    
    /* Инициализируем I2S заново с частотой 44.1 кГц */
    i2s_init();
}

bool is_supported_ext(const char *ext)
{
    if (!ext) return false;
    return (strcasecmp(ext, ".xm") == 0 ||
            strcasecmp(ext, ".mod") == 0 ||
            strcasecmp(ext, ".s3m") == 0 ||
            strcasecmp(ext, ".it") == 0 ||
            strcasecmp(ext, ".stm") == 0 ||
            strcasecmp(ext, ".669") == 0 ||
            strcasecmp(ext, ".far") == 0 ||
            strcasecmp(ext, ".fnk") == 0 ||
            strcasecmp(ext, ".imf") == 0 ||
            strcasecmp(ext, ".liq") == 0 ||
            strcasecmp(ext, ".mdl") == 0 ||
            strcasecmp(ext, ".mtm") == 0 ||
            strcasecmp(ext, ".ptm") == 0 ||
            strcasecmp(ext, ".rtm") == 0 ||
            strcasecmp(ext, ".ult") == 0 ||
            strcasecmp(ext, ".amf") == 0 ||
            strcasecmp(ext, ".gdm") == 0 ||
            strcasecmp(ext, ".psm") == 0 ||
            strcasecmp(ext, ".j2b") == 0 ||
            strcasecmp(ext, ".mfp") == 0 ||
            strcasecmp(ext, ".smp") == 0 ||
            strcasecmp(ext, ".mmdc") == 0 ||
            strcasecmp(ext, ".stim") == 0 ||
            strcasecmp(ext, ".umx") == 0 ||
            strcasecmp(ext, ".xmf") == 0 ||
            strcasecmp(ext, ".dbm") == 0 ||
            strcasecmp(ext, ".digi") == 0 ||
            strcasecmp(ext, ".emod") == 0 ||
            strcasecmp(ext, ".med") == 0 ||
            strcasecmp(ext, ".mtn") == 0 ||
            strcasecmp(ext, ".okt") == 0 ||
            strcasecmp(ext, ".sfx") == 0 ||
            strcasecmp(ext, ".dtm") == 0 ||
            strcasecmp(ext, ".mgt") == 0 ||
            strcasecmp(ext, ".dmf") == 0 ||
            strcasecmp(ext, ".wow") == 0 ||
            strcasecmp(ext, ".flx") == 0 ||
            strcasecmp(ext, ".abk") == 0 ||
            strcasecmp(ext, ".stx") == 0 ||
            strcasecmp(ext, ".vgm") == 0 ||
            strcasecmp(ext, ".vgz") == 0 ||
            strcasecmp(ext, ".nsf") == 0 ||
            strcasecmp(ext, ".gbs") == 0 ||
            strcasecmp(ext, ".spc") == 0 ||
            strcasecmp(ext, ".gym") == 0 ||
            strcasecmp(ext, ".wav") == 0 ||
            strcasecmp(ext, ".mp3") == 0 ||
            strcasecmp(ext, ".opus") == 0 ||
            strcasecmp(ext, ".mid") == 0 ||
            strcasecmp(ext, ".midi") == 0 ||
            strcasecmp(ext, ".ay") == 0 ||
            strcasecmp(ext, ".sf2") == 0 ||
            strcasecmp(ext, ".gb") == 0 ||
            strcasecmp(ext, ".gbc") == 0 ||
            strcasecmp(ext, ".flac") == 0);
}

void scan_playlist(const char *track_path)
{
    char dir_path[256];

    if (strncmp(track_path, "A:", 2) == 0) {
        snprintf(dir_path, sizeof(dir_path), "/sdcard%s", track_path + 2);
    } else {
        strncpy(dir_path, track_path, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
    }

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) *last_slash = '\0';

    playlist_count = 0;
    ESP_LOGI(TAG, "Scanning dir: %s", dir_path);

    DIR *dir = opendir(dir_path);
    if (!dir) { ESP_LOGW(TAG, "Cannot open dir: %s", dir_path); return; }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && playlist_count < 32) {
        if (entry->d_type != DT_REG) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (is_supported_ext(ext)) {
            int written = snprintf(playlist[playlist_count], 256, "%s/%.200s", dir_path, entry->d_name);
            if (written < 0 || written >= 256) continue;
            if (strstr(track_path, entry->d_name) || strstr(dir_path, entry->d_name))
                current_track_index = playlist_count;
            playlist_count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Playlist: %d tracks (current: %d)", playlist_count, current_track_index);
}

/* Проверка GameBoy ROM файла */
static bool is_gb_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".gb") == 0 ||
            strcasecmp(ext, ".gbc") == 0);
}

/* Проверка MIDI файла */
static bool is_midi_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".mid") == 0 ||
            strcasecmp(ext, ".midi") == 0);
}

/* Проверка USB-MIDI файла */
static bool is_usb_midi_file(const char *filename)
{
    const char *name = strrchr(filename, '/');
    if (!name) name = filename;
    else name++; // пропускаем слеш
    
    // Проверяем, что файл называется MidiSynth.usb
    return (strcasecmp(name, "MidiSynth.usb") == 0);
}

/* Проверка GME файла (игровая музыка) */
static bool is_gme_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".vgm") == 0 ||
            strcasecmp(ext, ".vgz") == 0 ||
            strcasecmp(ext, ".nsf") == 0 ||
            strcasecmp(ext, ".gbs") == 0 ||
            strcasecmp(ext, ".spc") == 0 ||
            strcasecmp(ext, ".ay") == 0 ||
            strcasecmp(ext, ".gym") == 0);
}


/* Проверка WAV */
static bool is_wav_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".wav") == 0;
}

/* Проверка FLAC */
static bool is_flac_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".flac") == 0;
}
/* Проверка МР3 */
static bool is_mp3_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".mp3") == 0;
}
/* Проверка OPUS */
static bool is_opus_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".opus") == 0;
}


/* Программная регулировка громкости */
static void apply_volume(int16_t *buffer, size_t samples, int volume)
{
    if (volume >= 100) return;
    int32_t mult = (volume * 32767) / 100;
    for (size_t i = 0; i < samples; i++) {
        int32_t s = buffer[i];
        s = ((s * mult) + 16384) >> 15;
        buffer[i] = (int16_t)s;
    }
}

/* Ресемплинг PCM */
static size_t resample_pcm(const int16_t *input, size_t input_samples, 
                            int16_t *output, size_t output_max,
                            uint32_t src_rate, uint32_t dst_rate)
{
    if (src_rate == dst_rate) {
        if (input_samples <= output_max) {
            memcpy(output, input, input_samples * sizeof(int16_t));
            return input_samples;
        }
        memcpy(output, input, output_max * sizeof(int16_t));
        return output_max;
    }
    
    double ratio = (double)src_rate / dst_rate;
    size_t expected_output = (size_t)((double)input_samples / ratio);
    if (expected_output > output_max) expected_output = output_max;
    
    for (size_t out_idx = 0; out_idx < expected_output; out_idx++) {
        double src_pos = out_idx * ratio;
        size_t idx = (size_t)src_pos;
        double frac = src_pos - idx;
        
        if (idx + 1 < input_samples) {
            int32_t sample = (int32_t)(input[idx] * (1.0 - frac) + input[idx + 1] * frac);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            output[out_idx] = (int16_t)sample;
        } else if (idx < input_samples) {
            output[out_idx] = input[idx];
        } else {
            output[out_idx] = 0;
        }
    }
    return expected_output;
}

/* --- Загрузка трека для libxmp --- */
static esp_err_t load_new_track(xmp_context ctx, const char *real_path, char *title_out, size_t title_size, char *tracker_out, size_t tracker_size)
{
    ESP_LOGI(TAG, "Loading: %s", real_path);
    
    FILE *file = fopen(real_path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;

    esp_xmp_file_t *f_priv = (esp_xmp_file_t *)heap_caps_malloc(sizeof(esp_xmp_file_t), MALLOC_CAP_SPIRAM);
    if (!f_priv) { fclose(file); return ESP_ERR_NO_MEM; }
    f_priv->file = file; 
    f_priv->should_free = 1;

    struct xmp_callbacks callbacks = { cb_read, cb_seek, cb_tell, cb_close };
    int load_res = xmp_load_module_from_callbacks(ctx, f_priv, callbacks);
    if (load_res != 0) { 
        ESP_LOGE(TAG, "xmp_load_module failed: error code %d", load_res);
        fclose(file);
        vTaskDelay(pdMS_TO_TICKS(10));
        heap_caps_free(f_priv); 
        return ESP_ERR_INVALID_ARG; 
    }

    struct xmp_module_info info;
    xmp_get_module_info(ctx, &info);
    
    if (info.mod->name[0]) {
        strncpy(title_out, info.mod->name, title_size - 1);
    } else {
        const char *fname = strrchr(real_path, '/');
        fname = fname ? fname + 1 : real_path;
        strncpy(title_out, fname, title_size - 1);
    }
    title_out[title_size - 1] = '\0';

    const char *tracker = info.mod->type[0] ? info.mod->type : "???";
    strncpy(tracker_out, tracker, tracker_size - 1);
    tracker_out[tracker_size - 1] = '\0';

    if (xmp_start_player(ctx, 44100, 0) < 0) { 
        xmp_release_module(ctx); 
        return ESP_ERR_INVALID_ARG; 
    }
    xmp_set_player(ctx, XMP_PLAYER_VOLUME, current_volume);
    ESP_LOGI(TAG, "Playing: %s [%s]", title_out, tracker_out);
    return ESP_OK;
}

/* --- WAV плеер --- */
static void wav_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_WAV;
    const char *filepath = (const char *)pvParameters;
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    ESP_LOGI(TAG, "WAV: playing %s", real_path);

    FILE *f = fopen(real_path, "rb");
    if (!f) { is_playing = false; vTaskDelete(NULL); return; }

    /* WAV заголовок */
    #pragma pack(push, 1)
    struct { 
        char riff[4]; 
        uint32_t size; 
        char wave[4]; 
        char fmt[4]; 
        uint32_t fmt_size; 
        uint16_t fmt_tag; 
        uint16_t ch; 
        uint32_t sr; 
        uint32_t br; 
        uint16_t ba; 
        uint16_t bps; 
        char data[4]; 
        uint32_t ds; 
    } hdr;
    #pragma pack(pop)
    
    fread(&hdr, sizeof(hdr), 1, f);
    
    /* Проверка корректности WAV файла */
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file");
        fclose(f);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "WAV: %d Hz, %d bits, %d ch", hdr.sr, hdr.bps, hdr.ch);

    /* Настраиваем I2S на частоту WAV файла */
    i2s_reconfigure(hdr.sr);

    /* Плейлист */
    char wav_playlist[32][256];
    int wav_count = 0, wav_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        ESP_LOGI(TAG, "WAV: scanning directory: %s", dir_path);
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && wav_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && strcasecmp(ext, ".wav") == 0) {
                        ESP_LOGI(TAG, "WAV: found file: %s", entry->d_name);
                        int w = snprintf(wav_playlist[wav_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) wav_index = wav_count;
                            wav_count++;
                        }
                    }
                }
            }
            closedir(dir);
            ESP_LOGI(TAG, "WAV: total found %d wav files", wav_count);
        } else {
            ESP_LOGE(TAG, "WAV: failed to open directory: %s", dir_path);
        }
    }

    /* Отправляем начальную информацию в UI */
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;

    ui_update_t ui = {0};
    ui.hide_loading = true;
    strncpy(ui.title, fname, sizeof(ui.title) - 1);
    ui.update_title = true;
    strncpy(ui.format, "WAV", sizeof(ui.format) - 1);
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    ui.update_module_info = true;
    ui.mod_channels = hdr.ch;
    ui.mod_bpm = hdr.sr;
    ui.mod_speed = (hdr.sr * hdr.ch * hdr.bps) / 1000;
    xQueueSend(ui_update_queue, &ui, 0);
    
    ESP_LOGI(TAG, "WAV: playing %s, %d Hz, %d ch", fname, hdr.sr, hdr.ch);

    #define WAV_BUF 4096
    uint8_t *buf = (uint8_t *)heap_caps_malloc(WAV_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) { 
        fclose(f); 
        is_playing = false; 
        vTaskDelete(NULL); 
        return; 
    }

    uint32_t rem = hdr.ds, elapsed_bytes = 0, last_ui = 0;
    uint32_t total_time_ms = (uint32_t)((uint64_t)hdr.ds * 1000 / hdr.br);
    bool stream_finished = false;

    while (is_playing && !stream_finished) {
        /* Обработка команд */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
                if (wav_count > 0) {
                    int next_index;
                    if (repeat_mode == REPEAT_SHUFFLE) {
                        next_index = esp_random() % wav_count;
                    } else if (cmd.type == PLAYER_CMD_NEXT_TRACK) {
                        next_index = (wav_index + 1) % wav_count;
                    } else {
                        next_index = (wav_index - 1 + wav_count) % wav_count;
                    }
                    
                    /* Освобождаем ресурсы */
                    if (buf) { heap_caps_free(buf); buf = NULL; }
                    if (f) { fclose(f); f = NULL; }
                    
                    /* Открываем новый файл */
                    f = fopen(wav_playlist[next_index], "rb");
                    if (f) {
                        ESP_LOGI(TAG, "WAV next: %s", wav_playlist[next_index]);
                        
                        /* Читаем заголовок нового файла */
                        fread(&hdr, sizeof(hdr), 1, f);
                        rem = hdr.ds;
                        elapsed_bytes = 0;
                        wav_index = next_index;
                        total_time_ms = (uint32_t)((uint64_t)hdr.ds * 1000 / hdr.br);
                        
                        /* Обновляем UI */
                        const char *new_fname = strrchr(wav_playlist[wav_index], '/');
                        new_fname = new_fname ? new_fname + 1 : wav_playlist[wav_index];
                        memset(&ui, 0, sizeof(ui));
                        strncpy(ui.title, new_fname, sizeof(ui.title)-1);
                        ui.update_title = true;
                        strncpy(ui.format, "WAV", sizeof(ui.format)-1);
                        ui.update_format = true;
                        ui.update_module_info = true;
                        ui.mod_channels = hdr.ch;
                        ui.mod_bpm = hdr.sr;
                        ui.mod_speed = (hdr.sr * hdr.ch * hdr.bps) / 1000;
                        xQueueSend(ui_update_queue, &ui, 0);
                        
                        /* Пересоздаём буфер */
                        buf = (uint8_t *)heap_caps_malloc(WAV_BUF, MALLOC_CAP_SPIRAM);
                        if (!buf) {
                            ESP_LOGE(TAG, "WAV: failed to allocate buffer");
                            stream_finished = true;
                            break;
                        }
                        
                        /* Перенастраиваем I2S */
                        i2s_reconfigure(hdr.sr);
                        last_ui = 0;
                    } else {
                        ESP_LOGE(TAG, "WAV: failed to open next track");
                        stream_finished = true;
                        break;
                    }
                }
            }
            else if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
        }
        
        /* Читаем и воспроизводим данные */
        size_t rd = fread(buf, 1, rem > WAV_BUF ? WAV_BUF : rem, f);
        if (rd == 0) {
            /* Конец файла - запускаем авто-переход */
            ESP_LOGI(TAG, "WAV: EOF reached, buffers empty");
            
            if (!stop_no_auto && wav_count > 0) {
                ESP_LOGI(TAG, "WAV: auto-next (reusing task)");
                
                int next_index;
                if (repeat_mode == REPEAT_ONE) {
                    next_index = wav_index;
                } else if (repeat_mode == REPEAT_SHUFFLE) {
                    next_index = esp_random() % wav_count;
                } else {
                    next_index = (wav_index + 1) % wav_count;
                }
                
                /* Полностью освобождаем ресурсы */
                if (buf) { heap_caps_free(buf); buf = NULL; }
                if (f) { fclose(f); f = NULL; }
                
                /* Открываем новый трек */
                f = fopen(wav_playlist[next_index], "rb");
                if (f) {
                    ESP_LOGI(TAG, "WAV: auto-next playing: %s", wav_playlist[next_index]);
                    
                    /* Читаем заголовок нового файла */
                    fread(&hdr, sizeof(hdr), 1, f);
                    
                    /* Обновляем UI */
                    const char *new_fname = strrchr(wav_playlist[next_index], '/');
                    new_fname = new_fname ? new_fname + 1 : wav_playlist[next_index];
                    memset(&ui, 0, sizeof(ui));
                    strncpy(ui.title, new_fname, sizeof(ui.title)-1);
                    ui.update_title = true;
                    strncpy(ui.format, "WAV", sizeof(ui.format)-1);
                    ui.update_format = true;
                    ui.update_module_info = true;
                    ui.mod_channels = hdr.ch;
                    ui.mod_bpm = hdr.sr;
                    ui.mod_speed = (hdr.sr * hdr.ch * hdr.bps) / 1000;
                    xQueueSend(ui_update_queue, &ui, 0);
                    
                    /* Пересоздаём буфер */
                    buf = (uint8_t *)heap_caps_malloc(WAV_BUF, MALLOC_CAP_SPIRAM);
                    if (!buf) {
                        ESP_LOGE(TAG, "WAV: failed to allocate buffer for auto-next");
                        fclose(f);
                        break;
                    }
                    
                    /* Сбрасываем состояние */
                    wav_index = next_index;
                    rem = hdr.ds;
                    elapsed_bytes = 0;
                    total_time_ms = (uint32_t)((uint64_t)hdr.ds * 1000 / hdr.br);
                    
                    /* Перенастраиваем I2S */
                    i2s_reconfigure(hdr.sr);
                    last_ui = 0;
                    
                    /* Продолжаем цикл */
                    continue;
                } else {
                    ESP_LOGE(TAG, "WAV: failed to open next track for auto-next");
                    stream_finished = true;
                    break;
                }
            } else {
                stream_finished = true;
                break;
            }
        }
        
        int16_t *pcm = (int16_t *)buf;
        size_t samples = rd / 2;
        
        /* Применяем громкость */
        apply_volume(pcm, samples, current_volume);
        
        /* Отправляем в I2S */
        size_t bytes = samples * sizeof(int16_t);
        size_t sent = 0;
        uint8_t *ptr = (uint8_t *)pcm;
        while (sent < bytes && is_playing) {
            size_t wr;
            i2s_channel_write(tx_chan, ptr + sent, bytes - sent, &wr, portMAX_DELAY);
            sent += wr;
        }
        
        rem -= rd;
        elapsed_bytes += rd;
        
        /* Обновляем UI (раз в 500 мс) */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500) {
            last_ui = now;
            uint32_t elapsed_ms = (uint32_t)((uint64_t)elapsed_bytes * 1000 / hdr.br);
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = elapsed_ms;
            ui.total_time_ms = total_time_ms;
            ui.pattern = 0;
            ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
    }
    
    /* Очистка ресурсов */
    if (buf) { heap_caps_free(buf); buf = NULL; }
    if (f) { fclose(f); f = NULL; }
    
    /* Восстанавливаем I2S для следующих плееров */
    i2s_reset_to_default();
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}






/* Конвертация 24/32-битных сэмплов в 16-бит */
static size_t convert_to_16bit(const uint8_t *input, size_t input_bytes, 
                                int16_t *output, size_t output_max,
                                int bits_per_sample, int num_channels)
{
    size_t samples = input_bytes / (bits_per_sample / 8);
    if (samples > output_max) samples = output_max;
    
    if (bits_per_sample == 16) {
        memcpy(output, input, samples * sizeof(int16_t));
        return samples;
    }
    
    else if (bits_per_sample == 24) {
        const uint8_t *src = input;
        for (size_t i = 0; i < samples; i++) {
            /* 24-bit signed → 16-bit (берём старшие 16 бит) */
            int32_t val = (src[0] << 8) | (src[1] << 16) | (src[2] << 24);
            output[i] = (int16_t)(val >> 16);
            src += 3;
        }
        return samples;
    }
    else if (bits_per_sample == 32) {
        const int32_t *src = (const int32_t *)input;
        for (size_t i = 0; i < samples; i++) {
            output[i] = (int16_t)(src[i] >> 16);
        }
        return samples;
    }
    
    return 0;  // Неподдерживаемый формат
}


/* --- FLAC плеер --- */
static void flac_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_FLAC;
    const char *filepath = (const char *)pvParameters;
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    FILE *f = fopen(real_path, "rb");
    if (!f) { is_playing = false; vTaskDelete(NULL); return; }

    /* Полный сброс I2S */
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        vTaskDelay(pdMS_TO_TICKS(100));
        i2s_channel_enable(tx_chan);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    micro_flac::FLACDecoder *decoder = new micro_flac::FLACDecoder();
    
    #define FLAC_INBUF 8192
    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(FLAC_INBUF, MALLOC_CAP_SPIRAM);
    size_t inbuf_len = 0;
    uint8_t *outbuf = nullptr;
    size_t out_size = 0;
    
    uint32_t sr = 44100, total = 0;
    uint8_t ch = 2;
    uint32_t elapsed = 0, last_ui = 0;

    #define FLAC_PCM_BUF 10000
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(FLAC_PCM_BUF * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    size_t pcm_buf_samples = 0;
    memset(pcm_buf, 0, FLAC_PCM_BUF * sizeof(int16_t));

    int16_t *rs_buf = NULL;
    int16_t *il_buf = NULL;

    ui_update_t ui = {0};
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;

    /* ★ ОТПРАВЛЯЕМ НАЗВАНИЕ ПЕРВОГО ТРЕКА ★ */
    memset(&ui, 0, sizeof(ui));
    ui.hide_loading = true;
    strncpy(ui.title, fname, sizeof(ui.title)-1);
    ui.update_title = true;
    strncpy(ui.format, "FLAC", sizeof(ui.format)-1);
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    xQueueSend(ui_update_queue, &ui, 0);
    
    ESP_LOGI(TAG, "FLAC: playing %s", fname);

    /* Плейлист */
    char flac_playlist[32][256];
    int flac_count = 0, flac_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && flac_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && strcasecmp(ext, ".flac") == 0) {
                        int w = snprintf(flac_playlist[flac_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) flac_index = flac_count;
                            flac_count++;
                        }
                    }
                }
            }
            closedir(dir);
        }
    }
    ESP_LOGI(TAG, "FLAC playlist: %d tracks, current=%d", flac_count, flac_index);

    /* Предварительное чтение */
    inbuf_len = fread(inbuf, 1, FLAC_INBUF, f);
    ESP_LOGI(TAG, "FLAC: read %d bytes initial", (int)inbuf_len);

    bool buffer_primed = false;
    buffer_primed = false;

    while (is_playing) {
        taskYIELD();
        
        /* Команды */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
                if (flac_count > 0) {
                    if (cmd.type == PLAYER_CMD_NEXT_TRACK) {
                        if (repeat_mode == REPEAT_SHUFFLE)
                            flac_index = esp_random() % flac_count;
                        else
                            flac_index = (flac_index + 1) % flac_count;
                    }
                    else {
                        if (repeat_mode == REPEAT_SHUFFLE)
                            flac_index = esp_random() % flac_count;
                        else
                            flac_index = (flac_index - 1 + flac_count) % flac_count;
                    }
                    
                    fclose(f);
                    delete decoder;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
                    f = fopen(flac_playlist[flac_index], "rb");
                    if (f) {
                        ESP_LOGI(TAG, "FLAC next: %s", flac_playlist[flac_index]);
                        decoder = new micro_flac::FLACDecoder();
                        inbuf_len = fread(inbuf, 1, FLAC_INBUF, f);
                        elapsed = 0;
                        pcm_buf_samples = 0;
                        memset(pcm_buf, 0, FLAC_PCM_BUF * sizeof(int16_t));
                        if (outbuf) { heap_caps_free(outbuf); outbuf = nullptr; }
                        if (rs_buf) { heap_caps_free(rs_buf); rs_buf = nullptr; }
                        if (il_buf) { heap_caps_free(il_buf); il_buf = nullptr; }
                        buffer_primed = false;
                        
                        fname = strrchr(flac_playlist[flac_index], '/');
                        fname = fname ? fname + 1 : flac_playlist[flac_index];
                        
                        memset(&ui, 0, sizeof(ui));
                        strncpy(ui.title, fname, sizeof(ui.title)-1);
                        ui.update_title = true;
                        strncpy(ui.format, "FLAC", sizeof(ui.format)-1);
                        ui.update_format = true;
                        ui.update_module_info = true;
                        ui.mod_channels = ch;
                        ui.mod_bpm = sr;
                        xQueueSend(ui_update_queue, &ui, 0);
                    }
                }
            }
            else if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
        }

        /* Декодируем */
        while (pcm_buf_samples < FLAC_PCM_BUF - 4096) {
            size_t consumed = 0, decoded = 0;
            auto res = decoder->decode(inbuf, inbuf_len, outbuf, out_size, consumed, decoded);

            if (consumed > 0) {
                memmove(inbuf, inbuf + consumed, inbuf_len - consumed);
                inbuf_len -= consumed;
            }

            if (inbuf_len < FLAC_INBUF / 2) {
                size_t rd = fread(inbuf + inbuf_len, 1, FLAC_INBUF - inbuf_len, f);
                if (rd > 0) inbuf_len += rd;
            }

            if (res == micro_flac::FLAC_DECODER_HEADER_READY) {
                auto &info = decoder->get_stream_info();
                sr = info.sample_rate(); 
                ch = info.num_channels();
                total = info.total_samples_per_channel();

                i2s_reconfigure(sr);

                out_size = info.max_block_size() * ch * info.bytes_per_sample();
                outbuf = (uint8_t *)heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
                
                size_t max_samp = info.max_block_size() * ch * 2;
                rs_buf = (int16_t *)heap_caps_malloc(max_samp * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                
                ESP_LOGI(TAG, "FLAC: %d Hz, %d ch, %d bps", sr, ch, info.bits_per_sample());
                
                /* Отправляем только модульную информацию */
                memset(&ui, 0, sizeof(ui));
                ui.update_module_info = true;
                ui.mod_channels = ch;
                ui.mod_bpm = sr;
                
                /* Расчёт среднего битрейта из размера файла */
                long file_pos = ftell(f);
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, file_pos, SEEK_SET);
                
                if (total > 0 && sr > 0) {
                    float duration_sec = (float)total / sr;
                    ui.mod_speed = (int)((file_size * 8) / (duration_sec * 1000));
                } else {
                    ui.mod_speed = 0;
                }
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (res == micro_flac::FLAC_DECODER_SUCCESS && outbuf) {
                size_t total_samples = decoded;
                size_t frames = total_samples / ch;
                elapsed += frames;
                
                int16_t *pcm = (int16_t *)outbuf;
                size_t to_copy = total_samples;
                if (pcm_buf_samples + to_copy > FLAC_PCM_BUF) {
                    to_copy = FLAC_PCM_BUF - pcm_buf_samples;
                }
                if (to_copy > 0) {
                    memcpy(pcm_buf + pcm_buf_samples, pcm, to_copy * sizeof(int16_t));
                    pcm_buf_samples += to_copy;
                }
            }
            else if (res == micro_flac::FLAC_DECODER_END_OF_STREAM) {
                ESP_LOGI(TAG, "FLAC: end of stream detected");
                
                /* Авто-переход */
                if (!stop_no_auto && flac_count > 0) {
                    ESP_LOGI(TAG, "FLAC: auto-next (reusing task)");
                    
                    int next_index;
                    if (repeat_mode == REPEAT_ONE) {
                        next_index = flac_index;
                    } else if (repeat_mode == REPEAT_SHUFFLE) {
                        next_index = esp_random() % flac_count;
                    } else {
                        next_index = (flac_index + 1) % flac_count;
                    }
                    
                    /* Закрываем текущие ресурсы */
                    if (outbuf) { heap_caps_free(outbuf); outbuf = nullptr; }
                    if (rs_buf) { heap_caps_free(rs_buf); rs_buf = nullptr; }
                    if (il_buf) { heap_caps_free(il_buf); il_buf = nullptr; }
                    if (pcm_buf) { 
                        memset(pcm_buf, 0, FLAC_PCM_BUF * sizeof(int16_t));
                        pcm_buf_samples = 0;
                    }
                    
                    /* Открываем новый трек */
                    fclose(f);
                    f = fopen(flac_playlist[next_index], "rb");
                    if (f) {
                        ESP_LOGI(TAG, "FLAC: auto-next playing: %s", flac_playlist[next_index]);
                        
                        /* Сбрасываем состояние */
                        flac_index = next_index;
                        elapsed = 0;
                        pcm_buf_samples = 0;
                        buffer_primed = false;
                        sr = 0;
                        ch = 0;
                        total = 0;
                        
                        /* Обновляем название в UI */
                        const char *new_fname = strrchr(flac_playlist[flac_index], '/');
                        fname = new_fname ? new_fname + 1 : flac_playlist[flac_index];
                        
                        memset(&ui, 0, sizeof(ui));
                        strncpy(ui.title, fname, sizeof(ui.title)-1);
                        ui.update_title = true;
                        strncpy(ui.format, "FLAC", sizeof(ui.format)-1);
                        ui.update_format = true;
                        ui.hide_loading = true;
                        ui.volume = current_volume;
                        ui.update_volume = true;
                        ui.mode = repeat_mode;
                        ui.update_mode = true;
                        xQueueSend(ui_update_queue, &ui, 0);
                        
                        /* Пересоздаём декодер */
                        delete decoder;
                        decoder = new micro_flac::FLACDecoder();
                        
                        /* Читаем начальные данные нового файла */
                        inbuf_len = fread(inbuf, 1, FLAC_INBUF, f);
                        
                        /* Продолжаем цикл */
                        continue;
                    } else {
                        ESP_LOGE(TAG, "FLAC: failed to open next track");
                        goto flac_done;
                    }
                } else {
                    goto flac_done;
                }
            }
            else if (res == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
                size_t rd = fread(inbuf + inbuf_len, 1, FLAC_INBUF - inbuf_len, f);
                if (rd > 0) {
                    inbuf_len += rd;
                } else if (inbuf_len == 0) {
                    ESP_LOGI(TAG, "FLAC: EOF reached, no more data");
                    
                    /* Авто-переход */
                    if (!stop_no_auto && flac_count > 0) {
                        ESP_LOGI(TAG, "FLAC: auto-next from NEED_MORE_DATA");
                        
                        int next_index;
                        if (repeat_mode == REPEAT_ONE) {
                            next_index = flac_index;
                        } else if (repeat_mode == REPEAT_SHUFFLE) {
                            next_index = esp_random() % flac_count;
                        } else {
                            next_index = (flac_index + 1) % flac_count;
                        }
                        
                        if (outbuf) { heap_caps_free(outbuf); outbuf = nullptr; }
                        if (rs_buf) { heap_caps_free(rs_buf); rs_buf = nullptr; }
                        if (il_buf) { heap_caps_free(il_buf); il_buf = nullptr; }
                        if (pcm_buf) { 
                            memset(pcm_buf, 0, FLAC_PCM_BUF * sizeof(int16_t));
                            pcm_buf_samples = 0;
                        }
                        
                        fclose(f);
                        f = fopen(flac_playlist[next_index], "rb");
                        if (f) {
                            flac_index = next_index;
                            elapsed = 0;
                            pcm_buf_samples = 0;
                            buffer_primed = false;
                            sr = 0;
                            ch = 0;
                            total = 0;
                            
                            const char *new_fname = strrchr(flac_playlist[flac_index], '/');
                            fname = new_fname ? new_fname + 1 : flac_playlist[flac_index];
                            
                            memset(&ui, 0, sizeof(ui));
                            strncpy(ui.title, fname, sizeof(ui.title)-1);
                            ui.update_title = true;
                            strncpy(ui.format, "FLAC", sizeof(ui.format)-1);
                            ui.update_format = true;
                            ui.hide_loading = true;
                            ui.volume = current_volume;
                            ui.update_volume = true;
                            ui.mode = repeat_mode;
                            ui.update_mode = true;
                            xQueueSend(ui_update_queue, &ui, 0);
                            
                            delete decoder;
                            decoder = new micro_flac::FLACDecoder();
                            inbuf_len = fread(inbuf, 1, FLAC_INBUF, f);
                            continue;
                        }
                    }
                    goto flac_done;
                }
            }
            else break;
        }

        /* Отправляем в I2S */
        if (!buffer_primed && pcm_buf_samples < FLAC_PCM_BUF / 2) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        buffer_primed = true;
        
        if (pcm_buf_samples > 0) {
            apply_volume(pcm_buf, pcm_buf_samples, current_volume);
            size_t bytes = pcm_buf_samples * sizeof(int16_t);
            size_t sent = 0;
            uint8_t *ptr = (uint8_t *)pcm_buf;
            while (sent < bytes && is_playing) {
                size_t chunk = (bytes - sent) > 2048 ? 2048 : (bytes - sent);
                size_t wr;
                i2s_channel_write(tx_chan, ptr + sent, chunk, &wr, pdMS_TO_TICKS(100));
                sent += wr;
                if (wr == 0) vTaskDelay(pdMS_TO_TICKS(2));
            }
            pcm_buf_samples = 0;
        }

        /* UI */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500 && sr > 0) {
            last_ui = now;
            uint32_t tms = (uint32_t)((uint64_t)elapsed * 1000 / sr);
            uint32_t tt = total ? (uint32_t)((uint64_t)total * 1000 / sr) : 0;
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = tms; ui.total_time_ms = tt;
            ui.pattern = 0; ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
    }

flac_done:
    ESP_LOGI(TAG, "FLAC: finished - elapsed=%us", (unsigned)(elapsed / sr));
        stop_no_auto = false;
    if (rs_buf) { heap_caps_free(rs_buf); rs_buf = NULL; }
    if (il_buf) { heap_caps_free(il_buf); il_buf = NULL; }
    if (pcm_buf) { heap_caps_free(pcm_buf); pcm_buf = NULL; }
    if (inbuf) { heap_caps_free(inbuf); inbuf = NULL; }
    if (outbuf) { heap_caps_free(outbuf); outbuf = NULL; }
    if (decoder) { delete decoder; decoder = NULL; }
    if (f) { fclose(f); f = NULL; }
    i2s_reset_to_default();
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}

static void mp3_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_MP3;
    const char *filepath = (const char *)pvParameters;
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    FILE *f = fopen(real_path, "rb");
    if (!f) { is_playing = false; vTaskDelete(NULL); return; }
// сброс i2s
    i2s_channel_disable(tx_chan);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_enable(tx_chan);

    micro_mp3::Mp3Decoder *decoder = new micro_mp3::Mp3Decoder();
    
    static const micro_mp3::Mp3Equalizer eq_map[] = {
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT,
    micro_mp3::MP3_EQ_FLAT
    };
    decoder->set_equalizer(eq_map[current_eq]);
    ESP_LOGI(TAG, "MP3 decoder EQ set to: %d", current_eq);

    #define MP3_INBUF 4096
    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(MP3_INBUF, MALLOC_CAP_SPIRAM);
    size_t inbuf_len = fread(inbuf, 1, MP3_INBUF, f);
    
    #define MP3_PCM_BUF 10000
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(MP3_PCM_BUF * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    size_t pcm_buf_samples = 0;
    memset(pcm_buf, 0, MP3_PCM_BUF * sizeof(int16_t));
    
    /* Выходной буфер для MP3 (фиксированный размер) */
    int16_t *outbuf = (int16_t *)heap_caps_malloc(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    
    uint32_t sr = 0;
    uint8_t ch = 0;
    uint32_t elapsed_samples = 0, last_ui = 0;
    uint32_t total_time_ms = 0;  /* ← ДОБАВИТЬ */
    bool info_ready = false;
    
    /* Плейлист */
    char mp3_playlist[32][256];
    int mp3_count = 0, mp3_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && mp3_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && strcasecmp(ext, ".mp3") == 0) {
                        int w = snprintf(mp3_playlist[mp3_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) mp3_index = mp3_count;
                            mp3_count++;
                        }
                    }
                }
            }
            closedir(dir);
        }
    }
    
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    ui_update_t ui = {0};
    ui.hide_loading = true;
    strncpy(ui.title, fname, sizeof(ui.title)-1);
    ui.update_title = true;
    strncpy(ui.format, "MP3", sizeof(ui.format)-1);
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    xQueueSend(ui_update_queue, &ui, 0);
    
    ESP_LOGI(TAG, "MP3: playing %s", fname);

    while (is_playing) {
        taskYIELD();
        
        /* Команды */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
                if (mp3_count > 0) {
                    if (repeat_mode == REPEAT_SHUFFLE)
                        mp3_index = esp_random() % mp3_count;
                    else if (cmd.type == PLAYER_CMD_NEXT_TRACK)
                        mp3_index = (mp3_index + 1) % mp3_count;
                    else
                        mp3_index = (mp3_index - 1 + mp3_count) % mp3_count;
                    
                    fclose(f);
                    f = fopen(mp3_playlist[mp3_index], "rb");
                    if (f) {
                        ESP_LOGI(TAG, "MP3 next: %s", mp3_playlist[mp3_index]);
                        delete decoder;
                        decoder = new micro_mp3::Mp3Decoder();
                        decoder->set_equalizer(eq_map[current_eq]);
                        ESP_LOGI(TAG, "MP3 EQ set to: %d", current_eq);
                        inbuf_len = fread(inbuf, 1, MP3_INBUF, f);
                        elapsed_samples = 0;
                        pcm_buf_samples = 0;
                        info_ready = false;
                        sr = 0;
                        
                        fname = strrchr(mp3_playlist[mp3_index], '/');
                        fname = fname ? fname + 1 : mp3_playlist[mp3_index];
                        
                        memset(&ui, 0, sizeof(ui));
                        strncpy(ui.title, fname, sizeof(ui.title)-1);
                        ui.update_title = true;
                        strncpy(ui.format, "MP3", sizeof(ui.format)-1);
                        ui.update_format = true;
                        xQueueSend(ui_update_queue, &ui, 0);
                    }
                }
            }
            else if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
        }
        
        /* Декодирование */
        if (inbuf_len > 0) {
            size_t consumed = 0, decoded = 0;
            auto res = decoder->decode(inbuf, inbuf_len, (uint8_t*)outbuf, 
                                       micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES, consumed, decoded);
            
            if (consumed > 0) {
                memmove(inbuf, inbuf + consumed, inbuf_len - consumed);
                inbuf_len -= consumed;
            }
            
            if (res == micro_mp3::MP3_STREAM_INFO_READY) {
                sr = decoder->get_sample_rate();
                ch = decoder->get_channels();
                ESP_LOGI(TAG, "MP3: %d Hz, %d ch", sr, ch);
                
                i2s_reconfigure(sr);
                
                /* Считаем общее время трека */
                long file_size = 0;
                long current_pos = ftell(f);
                fseek(f, 0, SEEK_END);
                file_size = ftell(f);
                fseek(f, current_pos, SEEK_SET);
                
                /* Примерное время = размер файла / битрейт (в секундах) */
                uint32_t bitrate = decoder->get_bitrate();
                if (bitrate > 0) {
                    uint32_t total_ms = (uint32_t)((file_size * 8) / bitrate);  // битрейт в kbps -> ms
                    total_time_ms = total_ms;
                } else {
                    total_time_ms = 0;
                }
                
                ESP_LOGI(TAG, "MP3: bitrate=%dkbps, size=%ld, time=%ums", bitrate, file_size, total_time_ms);


                memset(&ui, 0, sizeof(ui));
                ui.update_module_info = true;
                ui.mod_channels = ch;
                ui.mod_bpm = sr;
                ui.mod_speed = decoder->get_bitrate();
                xQueueSend(ui_update_queue, &ui, 0);
                info_ready = true;
            }
            else if (res == micro_mp3::MP3_OK && decoded > 0 && info_ready) {
                size_t total_samples;
                
               
                /* MP3 декодер отдаёт УЖЕ интерливинговые данные */
                total_samples = decoded * ch;  // decoded = per channel, total = all channels
                
                elapsed_samples += decoded;
                
                /* Копируем в выходной буфер */
                size_t to_copy = total_samples;
                if (pcm_buf_samples + to_copy > MP3_PCM_BUF)
                    to_copy = MP3_PCM_BUF - pcm_buf_samples;
                if (to_copy > 0) {
                    memcpy(pcm_buf + pcm_buf_samples, outbuf, to_copy * sizeof(int16_t));
                    pcm_buf_samples += to_copy;
                }
            }
            else if (res == micro_mp3::MP3_DECODE_ERROR) {
                /* Пропускаем битый фрейм */
            }
        }
        
        /* Дозаполняем входной буфер */
        if (inbuf_len < MP3_INBUF / 2) {
            size_t rd = fread(inbuf + inbuf_len, 1, MP3_INBUF - inbuf_len, f);
            if (rd > 0) inbuf_len += rd;
        }
        
        /* Отправляем в I2S */
                if (pcm_buf_samples > 2048) {  // Чуть больше для 24-бит
            apply_volume(pcm_buf, pcm_buf_samples, current_volume);
            size_t bytes = pcm_buf_samples * sizeof(int16_t);
            size_t sent = 0;
            uint8_t *ptr = (uint8_t *)pcm_buf;
            while (sent < bytes && is_playing) {
                size_t chunk = (bytes - sent) > 2048 ? 2048 : (bytes - sent);
                size_t wr;
                i2s_channel_write(tx_chan, ptr + sent, chunk, &wr, pdMS_TO_TICKS(50));
                sent += wr;
                if (wr == 0) vTaskDelay(pdMS_TO_TICKS(2));
            }
            pcm_buf_samples = 0;
        }
        
        /* UI */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500 && sr > 0) {
            last_ui = now;
            uint32_t tms = (uint32_t)((uint64_t)elapsed_samples * 1000 / sr);
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = tms;
            ui.total_time_ms = total_time_ms;
            ui.pattern = 0; ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
        
        /* Конец файла */
        if (inbuf_len == 0 && pcm_buf_samples == 0) break;
    }
    
    /* Очистка */
    heap_caps_free(pcm_buf);
    heap_caps_free(inbuf);
    heap_caps_free(outbuf);
    delete decoder;
    fclose(f);
        
    i2s_reset_to_default();


    /* Авто-переход */
    if (!stop_no_auto && mp3_count > 0) {
        if (repeat_mode == REPEAT_ONE)
            audio_player_play(mp3_playlist[mp3_index]);
        else if (repeat_mode == REPEAT_SHUFFLE)
            audio_player_play(mp3_playlist[esp_random() % mp3_count]);
        else {
            mp3_index = (mp3_index + 1) % mp3_count;
            audio_player_play(mp3_playlist[mp3_index]);
        }
        vTaskDelete(NULL);
        return;
    }
    
    stop_no_auto = false;
    is_playing = false;
    player_stopped_for_explorer = true;
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}



/* --- OPUS плеер --- */
static void opus_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_OPUS;
    const char *filepath = (const char *)pvParameters;
    ESP_LOGI(TAG, "OPUS TASK STARTED for: %s", filepath);
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    FILE *f = fopen(real_path, "rb");
    if (!f) { is_playing = false; vTaskDelete(NULL); return; }

    i2s_reconfigure(48000);

    /* Сброс I2S */
    i2s_channel_disable(tx_chan);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_enable(tx_chan);

    /* Создаём OggOpusDecoder */
    micro_opus::OggOpusDecoder *decoder = new micro_opus::OggOpusDecoder(true, 48000, 2);
    
    #define OPUS_INBUF 16384
    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(OPUS_INBUF, MALLOC_CAP_SPIRAM);
    size_t inbuf_len = 0;
    
    /* Буфер для PCM будет выделен после получения первого аудио */
    int16_t *pcm_buf = NULL;
    size_t pcm_buf_max_samples = 0;
    size_t pcm_buf_samples = 0;
    
    /* Временный буфер для заголовков */
    int16_t temp_buf[4096];
    
    uint32_t sr = 48000;
    uint8_t ch = 2;
    uint32_t elapsed_samples = 0, last_ui = 0;
    uint32_t total_time_ms = 0;
    bool headers_parsed = false;
    bool stream_finished = false;
    
    /* Плейлист */
    char opus_playlist[32][256];
    int opus_count = 0, opus_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        ESP_LOGI(TAG, "OPUS: scanning directory: %s", dir_path);
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && opus_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && strcasecmp(ext, ".opus") == 0) {
                        ESP_LOGI(TAG, "OPUS: found file: %s", entry->d_name);
                        int w = snprintf(opus_playlist[opus_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) {
                                opus_index = opus_count;
                                ESP_LOGI(TAG, "OPUS: current track index = %d", opus_index);
                            }
                            opus_count++;
                        }
                    }
                }
            }
            closedir(dir);
            ESP_LOGI(TAG, "OPUS: total found %d opus files", opus_count);
        } else {
            ESP_LOGE(TAG, "OPUS: failed to open directory: %s", dir_path);
        }
    }
    
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;

    ui_update_t ui = {0};
    ui.hide_loading = true;
    strncpy(ui.title, fname, sizeof(ui.title)-1);
    ui.update_title = true;
    strncpy(ui.format, "OPUS", sizeof(ui.format)-1);
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    xQueueSend(ui_update_queue, &ui, 0);
    
    ESP_LOGI(TAG, "OPUS: playing %s", fname);

    /* Читаем начальные данные */
    inbuf_len = fread(inbuf, 1, OPUS_INBUF, f);
    size_t input_offset = 0;
    
    int no_progress_count = 0;

    while (is_playing && !stream_finished) {
        vTaskDelay(pdMS_TO_TICKS(5));
        
        /* Обработка команд */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
                if (opus_count > 0) {
                    if (repeat_mode == REPEAT_SHUFFLE)
                        opus_index = esp_random() % opus_count;
                    else if (cmd.type == PLAYER_CMD_NEXT_TRACK)
                        opus_index = (opus_index + 1) % opus_count;
                    else
                        opus_index = (opus_index - 1 + opus_count) % opus_count;
                    
                    fclose(f);
                    delete decoder;
                    if (pcm_buf) {
                        heap_caps_free(pcm_buf);
                        pcm_buf = NULL;
                    }
                    
                    f = fopen(opus_playlist[opus_index], "rb");
                    if (f) {
                        ESP_LOGI(TAG, "OPUS next: %s", opus_playlist[opus_index]);
                        decoder = new micro_opus::OggOpusDecoder(true, 48000, 2);
                        inbuf_len = fread(inbuf, 1, OPUS_INBUF, f);
                        input_offset = 0;
                        elapsed_samples = 0;
                        pcm_buf_samples = 0;
                        pcm_buf_max_samples = 0;
                        headers_parsed = false;
                        stream_finished = false;
                        no_progress_count = 0;
                        
                        fname = strrchr(opus_playlist[opus_index], '/');
                        fname = fname ? fname + 1 : opus_playlist[opus_index];
                        
                        memset(&ui, 0, sizeof(ui));
                        strncpy(ui.title, fname, sizeof(ui.title)-1);
                        ui.update_title = true;
                        strncpy(ui.format, "OPUS", sizeof(ui.format)-1);
                        ui.update_format = true;
                        xQueueSend(ui_update_queue, &ui, 0);
                    }
                }
            }
            else if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
        }
        
        /* Декодирование - вызываем ВСЕГДА, даже без буфера */
        if (inbuf_len > 0 && !stream_finished) {
            size_t bytes_consumed = 0, samples_decoded = 0;
            
            /* Выбираем выходной буфер */
            uint8_t *output_ptr = (uint8_t*)temp_buf;
            size_t output_size = sizeof(temp_buf);
            
            if (pcm_buf) {
                /* Используем основной буфер, если он есть */
                size_t available_samples = pcm_buf_max_samples - pcm_buf_samples;
                if (available_samples > 0) {
                    output_ptr = (uint8_t*)pcm_buf + pcm_buf_samples * sizeof(int16_t);
                    output_size = available_samples * sizeof(int16_t);
                } else {
                    /* Буфер полон, отправим в I2S позже */
                    output_size = 0;
                }
            }
            
            if (output_size > 0) {
                int res = decoder->decode(
                    inbuf + input_offset, inbuf_len - input_offset,
                    output_ptr, output_size,
                    bytes_consumed, samples_decoded);
                
                if (bytes_consumed > 0) {
                    input_offset += bytes_consumed;
                    if (input_offset >= inbuf_len) {
                        if (inbuf_len - input_offset > 0) {
                            memmove(inbuf, inbuf + input_offset, inbuf_len - input_offset);
                        }
                        inbuf_len -= input_offset;
                        input_offset = 0;
                        
                        if (!feof(f)) {
                            size_t rd = fread(inbuf + inbuf_len, 1, OPUS_INBUF - inbuf_len, f);
                            if (rd > 0) inbuf_len += rd;
                        }
                    }
                    no_progress_count = 0;
                } else {
                    no_progress_count++;
                    if (no_progress_count > 500 && feof(f) && inbuf_len == 0) {
                        ESP_LOGI(TAG, "OPUS: no progress, finishing");
                        stream_finished = true;
                        break;
                    }
                }
                
                                /* Обработка результата декодирования */
                if (res == 0 && samples_decoded > 0) {
                    /* samples_decoded - количество семплов НА КАНАЛ */
                    size_t total_samples = samples_decoded * ch;
                    size_t total_bytes = total_samples * sizeof(int16_t);
                    
                    if (!pcm_buf) {
                        ESP_LOGI(TAG, "OPUS: first audio decoded: %d samples per channel, total=%d samples", 
                                 (int)samples_decoded, (int)total_samples);
                        
                        pcm_buf_max_samples = 32000;
                        pcm_buf = (int16_t *)heap_caps_malloc(pcm_buf_max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                        if (pcm_buf) {
                            memcpy(pcm_buf, temp_buf, total_bytes);
                            pcm_buf_samples = total_samples;
                            elapsed_samples += total_samples;
                        }
                    } else {
                        pcm_buf_samples += total_samples;
                        elapsed_samples += total_samples;
                    }
                }
                
                /* Получаем информацию о потоке (ОТДЕЛЬНЫЙ БЛОК) */
                if (!headers_parsed && decoder->is_initialized()) {
                    sr = decoder->get_sample_rate();
                    ch = decoder->get_channels();
                    
                    /* Получаем размер файла */
                    long file_pos = ftell(f);
                    fseek(f, 0, SEEK_END);
                    long file_size = ftell(f);
                    fseek(f, file_pos, SEEK_SET);
                    
                    total_time_ms = 0;
                    
                    memset(&ui, 0, sizeof(ui));
                    ui.update_module_info = true;
                    ui.mod_channels = ch;
                    ui.mod_bpm = sr;
                    ui.mod_speed = 0;
                    xQueueSend(ui_update_queue, &ui, 0);
                    
                    headers_parsed = true;
                }
                
                /* Обработка конца потока и ошибок - на том же уровне */
                if (res == -2) {
                    ESP_LOGI(TAG, "OPUS: end of stream");
                    stream_finished = true;
                    break;
                }
                else if (res == -1) {
                    /* NEED_MORE_DATA */
                    if (feof(f) && inbuf_len == 0) {
                        stream_finished = true;
                        break;
                    }
                }
                else if (res < 0 && res != -1) {
                    ESP_LOGW(TAG, "OPUS decode error: %d", res);
                    input_offset += 256;
                    if (input_offset >= inbuf_len) {
                        input_offset = 0;
                        if (!feof(f)) {
                            inbuf_len = fread(inbuf, 1, OPUS_INBUF, f);
                        }
                    }
                }
            }
        }
        
        /* Отправляем в I2S */
        if (pcm_buf && pcm_buf_samples > 2048) {
            apply_volume(pcm_buf, pcm_buf_samples, current_volume);
            size_t bytes = pcm_buf_samples * sizeof(int16_t);
            size_t sent = 0;
            uint8_t *ptr = (uint8_t *)pcm_buf;
            while (sent < bytes && is_playing) {
                size_t chunk = (bytes - sent) > 2048 ? 2048 : (bytes - sent);
                size_t wr;
                i2s_channel_write(tx_chan, ptr + sent, chunk, &wr, pdMS_TO_TICKS(50));
                sent += wr;
                if (wr == 0) vTaskDelay(pdMS_TO_TICKS(2));
            }
            pcm_buf_samples = 0;
        }
        
                /* UI обновление */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500 && sr > 0 && headers_parsed) {
            last_ui = now;
            uint32_t tms = (uint32_t)(((uint64_t)elapsed_samples / ch) * 1000 / sr);
            
            /* Если ещё не знаем общую длительность, вычисляем её */
            if (total_time_ms == 0 && tms > 3000) {  // Через 3 секунды
                long current_pos = ftell(f);
                long file_size = 0;
                fseek(f, 0, SEEK_END);
                file_size = ftell(f);
                fseek(f, current_pos, SEEK_SET);
                
                if (current_pos > 0 && file_size > 0) {
                    /* Оценка длительности по прогрессу */
                    total_time_ms = (uint32_t)((uint64_t)tms * file_size / current_pos);
                    
                    /* Вычисляем реальный битрейт */
                    uint32_t bitrate = (uint32_t)(((uint64_t)file_size * 8 * 1000) / total_time_ms / 1000);
                    
                    ESP_LOGI(TAG, "OPUS: file_size=%ld, current_pos=%ld, tms=%u", file_size, current_pos, tms);
                    ESP_LOGI(TAG, "OPUS: estimated total_time=%u ms, bitrate=%d kbps", total_time_ms, bitrate);
                    
                    /* Обновляем UI с реальным битрейтом */
                    memset(&ui, 0, sizeof(ui));
                    ui.update_module_info = true;
                    ui.mod_channels = ch;
                    ui.mod_bpm = sr;
                    ui.mod_speed = bitrate;
                    xQueueSend(ui_update_queue, &ui, 0);
                }
            }
            
            /* Отправляем текущее время в UI */
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = tms;
            ui.total_time_ms = total_time_ms;
            ui.pattern = 0;
            ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
        
        /* Проверка конца файла и авто-переход */
        if (feof(f) && inbuf_len == 0 && pcm_buf_samples == 0) {
            ESP_LOGI(TAG, "OPUS: EOF reached, buffers empty");
            
            /* Авто-переход */
            if (!stop_no_auto && opus_count > 0) {
                ESP_LOGI(TAG, "OPUS: auto-next (reusing task)");
                
                int next_index;
                if (repeat_mode == REPEAT_ONE) {
                    next_index = opus_index;
                } else if (repeat_mode == REPEAT_SHUFFLE) {
                    next_index = esp_random() % opus_count;
                } else {
                    next_index = (opus_index + 1) % opus_count;
                }
                
                /* ★ ПОЛНОСТЬЮ ОСВОБОЖДАЕМ ВСЕ РЕСУРСЫ ★ */
                if (pcm_buf) { 
                    heap_caps_free(pcm_buf); 
                    pcm_buf = NULL; 
                }
                if (inbuf) { 
                    heap_caps_free(inbuf); 
                    inbuf = NULL; 
                }
                if (decoder) { 
                    delete decoder; 
                    decoder = NULL; 
                }
                if (f) { 
                    fclose(f); 
                    f = NULL; 
                }
                
                /* Открываем новый трек */
                f = fopen(opus_playlist[next_index], "rb");
                if (f) {
                    ESP_LOGI(TAG, "OPUS: auto-next playing: %s", opus_playlist[next_index]);
                    
                    /* Выделяем новые буферы */
                    inbuf = (uint8_t *)heap_caps_malloc(OPUS_INBUF, MALLOC_CAP_SPIRAM);
                    if (!inbuf) {
                        ESP_LOGE(TAG, "OPUS: failed to allocate inbuf");
                        break;
                    }
                    
                    /* Сбрасываем состояние */
                    opus_index = next_index;
                    input_offset = 0;
                    elapsed_samples = 0;
                    pcm_buf_samples = 0;
                    pcm_buf_max_samples = 0;
                    headers_parsed = false;
                    stream_finished = false;
                    no_progress_count = 0;
                    sr = 48000;
                    ch = 2;
                    total_time_ms = 0;
                    
                    /* Обновляем название в UI */
                    const char *new_fname = strrchr(opus_playlist[opus_index], '/');
                    new_fname = new_fname ? new_fname + 1 : opus_playlist[opus_index];
                    memset(&ui, 0, sizeof(ui));
                    strncpy(ui.title, new_fname, sizeof(ui.title)-1);
                    ui.update_title = true;
                    strncpy(ui.format, "OPUS", sizeof(ui.format)-1);
                    ui.update_format = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    
                    /* Пересоздаём декодер */
                    decoder = new micro_opus::OggOpusDecoder(true, 48000, 2);
                    
                    /* Читаем начальные данные нового файла */
                    inbuf_len = fread(inbuf, 1, OPUS_INBUF, f);
                    
                    /* Продолжаем цикл */
                    continue;
                } else {
                    ESP_LOGE(TAG, "OPUS: failed to open next track");
                    stream_finished = true;
                    break;
                }
            } else {
                stream_finished = true;
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "OPUS: EXITED LOOP - stream_finished=%d, is_playing=%d", 
             (int)stream_finished, (int)is_playing);
        stop_no_auto = false;
    /* Очистка */
    if (pcm_buf) { heap_caps_free(pcm_buf); pcm_buf = NULL; }
    if (inbuf) { heap_caps_free(inbuf); inbuf = NULL; }
    if (decoder) { delete decoder; decoder = NULL; }
    if (f) { fclose(f); f = NULL; }
    i2s_reset_to_default();
    ESP_LOGI(TAG, "OPUS: stop_no_auto=%d, opus_count=%d, repeat_mode=%d", 
             stop_no_auto, opus_count, repeat_mode);
    

    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}


/* --- GME плеер (максимально оптимизированный для ESP32) --- */
static void gme_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_GME;
    const char *filepath = (const char *)pvParameters;
    ESP_LOGI(TAG, "GME TASK STARTED for: %s", filepath);
    
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    /* Открываем файл */
    FILE *f = fopen(real_path, "rb");
    if (!f) { 
        is_playing = false; 
        vTaskDelete(NULL); 
        return; 
    }
    
    /* Читаем файл в память */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_data) {
        ESP_LOGE(TAG, "GME: failed to allocate file buffer");
        fclose(f);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    fread(file_data, 1, file_size, f);
    fclose(f);
    
    /* Определяем тип файла */
    gme_type_t file_type = gme_identify_extension(real_path);
    if (!file_type && file_size >= 4) {
        file_type = gme_identify_extension(gme_identify_header(file_data));
    }
    if (!file_type) {
        ESP_LOGE(TAG, "GME: unknown file type");
        heap_caps_free(file_data);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Создаём эмулятор */
    Music_Emu *emu = gme_new_emu(file_type, 44100);
    if (!emu) {
        ESP_LOGE(TAG, "GME: failed to create emulator");
        heap_caps_free(file_data);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Сохраняем указатель на текущий эмулятор для применения настроек */
    current_gme_emu = emu;
    
    /* Применяем сохранённые настройки GME */
    audio_player_apply_gme_settings();



    /* Загружаем данные */
    gme_err_t err = gme_load_data(emu, file_data, file_size);
    if (err) {
        ESP_LOGE(TAG, "GME: load error: %s", err);
        gme_delete(emu);
        heap_caps_free(file_data);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Настраиваем I2S на 44100 Гц */
    i2s_reconfigure(44100);
    
    /* Сброс I2S */
    i2s_channel_disable(tx_chan);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_enable(tx_chan);
    
    /* Плейлист */
    char gme_playlist[32][256];
    int gme_count = 0, gme_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        ESP_LOGI(TAG, "GME: scanning directory: %s", dir_path);
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && gme_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && (strcasecmp(ext, ".vgm") == 0 ||
                                strcasecmp(ext, ".vgz") == 0 ||
                                strcasecmp(ext, ".nsf") == 0 ||
                                strcasecmp(ext, ".gbs") == 0 ||
                                strcasecmp(ext, ".spc") == 0 ||
                                strcasecmp(ext, ".gym") == 0)) {
                        int w = snprintf(gme_playlist[gme_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) gme_index = gme_count;
                            gme_count++;
                        }
                    }
                }
            }
            closedir(dir);
            ESP_LOGI(TAG, "GME: total found %d files", gme_count);
        } else {
            ESP_LOGE(TAG, "GME: failed to open directory: %s", dir_path);
        }
    }
    
    /* Информация о треках */
    int track_count = gme_track_count(emu);
    if (track_count <= 0) track_count = 1;
    ESP_LOGI(TAG, "GME: file has %d tracks", track_count);
    
    /* Получаем информацию о первом треке */
    gme_info_t *info = NULL;
    gme_track_info(emu, &info, 0);
    
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    const char *song_title = (info && info->song && info->song[0]) ? info->song : fname;
    const char *game_name = (info && info->game && info->game[0]) ? info->game : "Unknown Game";
    const char *author_name = (info && info->author && info->author[0]) ? info->author : "Unknown Author";
    const char *system_name = (info && info->system && info->system[0]) ? info->system : "Unknown System";
    uint32_t total_time_ms = (info && info->length > 0) ? info->length : 0;
    
    ESP_LOGI(TAG, "GME: song='%s', game='%s', author='%s', system='%s'", 
             song_title, game_name, author_name, system_name);
    
    /* Отправляем UI */
    ui_update_t ui = {0};
    ui.hide_loading = true;
    snprintf(ui.title, sizeof(ui.title), "%.45s", song_title);
    ui.update_title = true;
    snprintf(ui.format, sizeof(ui.format), "GME");
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    ui.pattern = 0;
    ui.total_patterns = 0;
    
    /* ★ ОТПРАВЛЯЕМ GME ИНФОРМАЦИЮ ★ */
    ui.update_gme_info = true;
    strncpy(ui.gme_game, game_name, sizeof(ui.gme_game) - 1);
    ui.gme_game[sizeof(ui.gme_game) - 1] = '\0';
    strncpy(ui.gme_author, author_name, sizeof(ui.gme_author) - 1);
    ui.gme_author[sizeof(ui.gme_author) - 1] = '\0';
    strncpy(ui.gme_system, system_name, sizeof(ui.gme_system) - 1);
    ui.gme_system[sizeof(ui.gme_system) - 1] = '\0';
    

    
    xQueueSend(ui_update_queue, &ui, 0);
    
    gme_free_info(info);
    
    /* Запускаем трек */
    err = gme_start_track(emu, 0);
    if (err) {
        ESP_LOGE(TAG, "GME: start track error: %s", err);
        gme_delete(emu);
        heap_caps_free(file_data);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* ОЧЕНЬ МАЛЕНЬКИЙ буфер - 256 семплов (5.8 мс при 44.1 кГц) */
    #define GME_PCM_BUF_SIZE 2048
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(GME_PCM_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "GME: failed to allocate PCM buffer");
        gme_delete(emu);
        heap_caps_free(file_data);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    uint32_t elapsed_samples = 0, last_ui = 0;
    int current_track = 0;
    bool stream_finished = false;
    
    ESP_LOGI(TAG, "GME: playing %s", fname);
    
    while (is_playing && !stream_finished) {
        /* Задержка для предотвращения watchdog */
        vTaskDelay(pdMS_TO_TICKS(1));
        
        /* Обработка команд */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
                if (track_count > 1) {
                    if (cmd.type == PLAYER_CMD_NEXT_TRACK) {
                        current_track = (current_track + 1) % track_count;
                    } else {
                        current_track = (current_track - 1 + track_count) % track_count;
                    }
                    
                    gme_start_track(emu, current_track);
                    elapsed_samples = 0;
                    
                    gme_track_info(emu, &info, current_track);
                    song_title = (info && info->song && info->song[0]) ? info->song : fname;
                    game_name = (info && info->game && info->game[0]) ? info->game : "Unknown Game";
                    author_name = (info && info->author && info->author[0]) ? info->author : "Unknown Author";
                    system_name = (info && info->system && info->system[0]) ? info->system : "Unknown System";
                    total_time_ms = (info && info->length > 0) ? info->length : 0;
                    
                    memset(&ui, 0, sizeof(ui));
                    /* Отправляем название трека */
                    snprintf(ui.title, sizeof(ui.title), "%.45s", song_title);
                    ui.update_title = true;
                    
                    /* Отправляем GME информацию */
                    ui.update_gme_info = true;
                    strncpy(ui.gme_game, game_name, sizeof(ui.gme_game) - 1);
                    ui.gme_game[sizeof(ui.gme_game) - 1] = '\0';
                    strncpy(ui.gme_author, author_name, sizeof(ui.gme_author) - 1);
                    ui.gme_author[sizeof(ui.gme_author) - 1] = '\0';
                    strncpy(ui.gme_system, system_name, sizeof(ui.gme_system) - 1);
                    ui.gme_system[sizeof(ui.gme_system) - 1] = '\0';
                    
                    
                    xQueueSend(ui_update_queue, &ui, 0);
                    gme_free_info(info);


                    gme_free_info(info);
                } else if (gme_count > 1) {
                    /* Переключение на другой файл */
                    int next_index;
                    if (repeat_mode == REPEAT_SHUFFLE) {
                        next_index = esp_random() % gme_count;
                    } else if (cmd.type == PLAYER_CMD_NEXT_TRACK) {
                        next_index = (gme_index + 1) % gme_count;
                    } else {
                        next_index = (gme_index - 1 + gme_count) % gme_count;
                    }
                    
                    /* Загружаем новый файл */
                    gme_delete(emu);
                    heap_caps_free(file_data);
                    
                    FILE *new_f = fopen(gme_playlist[next_index], "rb");
                    if (new_f) {
                        fseek(new_f, 0, SEEK_END);
                        file_size = ftell(new_f);
                        fseek(new_f, 0, SEEK_SET);
                        file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                        if (file_data) {
                            fread(file_data, 1, file_size, new_f);
                            fclose(new_f);
                            
                            file_type = gme_identify_extension(gme_playlist[next_index]);
                            emu = gme_new_emu(file_type, 44100);
                            if (emu) {
                                gme_load_data(emu, file_data, file_size);
                                track_count = gme_track_count(emu);
                                if (track_count <= 0) track_count = 1;
                                current_track = 0;
                                gme_start_track(emu, 0);
                                elapsed_samples = 0;
                                gme_index = next_index;
                                
                                /* ★ ПОЛУЧАЕМ ИНФОРМАЦИЮ О ТРЕКЕ ★ */
                                gme_info_t *new_info = NULL;
                                gme_track_info(emu, &new_info, 0);
                                
                                const char *new_fname = strrchr(gme_playlist[gme_index], '/');
                                new_fname = new_fname ? new_fname + 1 : gme_playlist[gme_index];
                                
                                /* ★ ОБНОВЛЯЕМ НАЗВАНИЕ ТРЕКА ИЗ META-ДАННЫХ ★ */
                                const char *new_song_title = (new_info && new_info->song && new_info->song[0]) 
                                                              ? new_info->song : new_fname;
                                const char *new_game_name = (new_info && new_info->game && new_info->game[0]) 
                                                              ? new_info->game : "Unknown Game";
                                const char *new_author_name = (new_info && new_info->author && new_info->author[0]) 
                                                               ? new_info->author : "Unknown Author";
                                const char *new_system_name = (new_info && new_info->system && new_info->system[0]) 
                                                               ? new_info->system : "Unknown System";
                                total_time_ms = (new_info && new_info->length > 0) ? new_info->length : 0;
                                
                                ESP_LOGI(TAG, "GME: switched to file: %s, song='%s'", new_fname, new_song_title);
                                
                                memset(&ui, 0, sizeof(ui));
                                /* ★ ОТПРАВЛЯЕМ НАЗВАНИЕ ТРЕКА ★ */
                                snprintf(ui.title, sizeof(ui.title), "%.45s", new_song_title);
                                ui.update_title = true;
                                
                                /* ★ ОТПРАВЛЯЕМ GME ИНФОРМАЦИЮ ★ */
                                ui.update_gme_info = true;
                                strncpy(ui.gme_game, new_game_name, sizeof(ui.gme_game) - 1);
                                ui.gme_game[sizeof(ui.gme_game) - 1] = '\0';
                                strncpy(ui.gme_author, new_author_name, sizeof(ui.gme_author) - 1);
                                ui.gme_author[sizeof(ui.gme_author) - 1] = '\0';
                                strncpy(ui.gme_system, new_system_name, sizeof(ui.gme_system) - 1);
                                ui.gme_system[sizeof(ui.gme_system) - 1] = '\0';
                                
                                xQueueSend(ui_update_queue, &ui, 0);
                                gme_free_info(new_info);
                            }
                        }
                    }
                }
            }
            else if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        /* Декодируем маленький кусок PCM */
        err = gme_play(emu, GME_PCM_BUF_SIZE, pcm_buf);
        if (err) {
            ESP_LOGW(TAG, "GME: play error: %s", err);
        }
        
        elapsed_samples += GME_PCM_BUF_SIZE;
        
        /* Применяем громкость */
        apply_volume(pcm_buf, GME_PCM_BUF_SIZE, current_volume);
        
        /* Отправляем в I2S небольшими порциями */
        size_t bytes = GME_PCM_BUF_SIZE * sizeof(int16_t);
        size_t sent = 0;
        uint8_t *ptr = (uint8_t *)pcm_buf;
        while (sent < bytes && is_playing) {
            size_t wr;
            i2s_channel_write(tx_chan, ptr + sent, bytes - sent, &wr, pdMS_TO_TICKS(20));
            sent += wr;
            if (wr == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        
        /* Принудительное переключение контекста после каждого буфера */
        taskYIELD();
        
        /* Проверка окончания трека */
        if (gme_track_ended(emu)) {
            ESP_LOGI(TAG, "GME: track ended");
            
            if (!stop_no_auto) {
                if (track_count > 1 && current_track + 1 < track_count) {
                    current_track++;
                    gme_start_track(emu, current_track);
                    elapsed_samples = 0;
                    continue;
                } else if (gme_count > 1) {
                    int next_index = (gme_index + 1) % gme_count;
                    
                    gme_delete(emu);
                    heap_caps_free(file_data);
                    
                    FILE *new_f = fopen(gme_playlist[next_index], "rb");
                    if (new_f) {
                        fseek(new_f, 0, SEEK_END);
                        file_size = ftell(new_f);
                        fseek(new_f, 0, SEEK_SET);
                        file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                        if (file_data) {
                            fread(file_data, 1, file_size, new_f);
                            fclose(new_f);
                            
                            file_type = gme_identify_extension(gme_playlist[next_index]);
                            emu = gme_new_emu(file_type, 44100);
                            if (emu) {
                                gme_load_data(emu, file_data, file_size);
                                track_count = gme_track_count(emu);
                                if (track_count <= 0) track_count = 1;
                                current_track = 0;
                                gme_start_track(emu, 0);
                                elapsed_samples = 0;
                                gme_index = next_index;
                                continue;
                            }
                        }
                    }
                }
            }
            stream_finished = true;
            break;
        }
        
        /* UI обновление (раз в 500 мс) */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500 && total_time_ms > 0) {
            last_ui = now;
            uint32_t tms = gme_tell(emu);
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = tms;
            ui.total_time_ms = total_time_ms;
            ui.pattern = 0;
            ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
    }
    
    /* Очистка */
    if (pcm_buf) { heap_caps_free(pcm_buf); }
    if (emu) { gme_delete(emu); }
    if (file_data) { heap_caps_free(file_data); }
    
    i2s_reset_to_default();
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}




/* --- MIDI плеер --- */
static void midi_player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_MIDI;
    const char *filepath = (const char *)pvParameters;
    ESP_LOGI(TAG, "MIDI TASK STARTED for: %s", filepath);
    
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);
    
    /* ★ ВСЕГДА ПЕРЕСОЗДАЁМ I2S, НЕЗАВИСИМО ОТ СОСТОЯНИЯ ★ */
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    
    /* Пересоздаём audio_ringbuf если нужно */
    if (audio_ringbuf) {
        vRingbufferDelete(audio_ringbuf);
        audio_ringbuf = NULL;
    }
    
    /* Инициализируем I2S заново */
    esp_err_t ret = i2s_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIDI: I2S init failed");
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Настраиваем I2S на 44100 Гц */
    i2s_reconfigure(44100);
    
    /* Сброс I2S перед использованием */
    i2s_channel_disable(tx_chan);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_enable(tx_chan);
    

    /* Инициализируем MIDI плеер */
    if (g_synth == nullptr) {

/* Проверяем существование SF2 файла */
FILE *test = fopen(g_midi_player_sf2_path, "rb");
if (!test) {
    ESP_LOGE(TAG, "SF2 file not found: %s", g_midi_player_sf2_path);
    is_playing = false;
    vTaskDelete(NULL);
    return;
}
fclose(test);
        
        if (midi_player_init(g_midi_player_sf2_path)) {
                ESP_LOGI(TAG, "MIDI player initialized with SF2: %s", g_midi_player_sf2_path);
        } else {
            ESP_LOGE(TAG, "Failed to initialize MIDI player with SF2: %s", g_selected_sf2_path);
            is_playing = false;
            vTaskDelete(NULL);
            return;
        }
        
        /* ★ АНАЛИЗИРУЕМ MIDI И ЗАГРУЖАЕМ ТОЛЬКО НУЖНЫЕ ИНСТРУМЕНТЫ ★ */
        midi_program_usage_t usage[32];
        int usage_count = midi_get_program_usage(real_path, usage, 32);
        
        if (usage_count > 0) {
            ESP_LOGI(TAG, "MIDI uses %d programs", usage_count);
            uint16_t banks[32], programs[32];
            for (int i = 0; i < usage_count; i++) {
                banks[i] = usage[i].bank;
                programs[i] = usage[i].program;
            }
            
            /* Загружаем только нужные пресеты */
            if (!g_parser->parse_lazy(banks, programs, usage_count)) {
                ESP_LOGE(TAG, "Failed to parse SF2 with lazy loading");
                is_playing = false;
                vTaskDelete(NULL);
                return;
            }
        } else {
            /* Fallback: загружаем всё */
            if (!g_parser->parse()) {
                ESP_LOGE(TAG, "Failed to parse SF2");
                is_playing = false;
                vTaskDelete(NULL);
                return;
            }
        }
        
        /* Инициализируем синтезатор после загрузки сэмплов */
        if (!g_synth->init()) {
            ESP_LOGE(TAG, "Failed to initialize Synth");
            is_playing = false;
            vTaskDelete(NULL);
            return;
        }
    } else {
        /* Синтезатор уже есть, просто сбрасываем его состояние */
        ESP_LOGI(TAG, "MIDI player already initialized, resetting...");
        if (g_synth) {
            g_synth->GMReset();
        }
    }
    
    /* ========== ПЛЕЙЛИСТ ========== */
    char midi_playlist[32][256];
    int midi_count = 0;
    int midi_index = 0;
    
    {
        char dir_path[256];
        strncpy(dir_path, real_path, sizeof(dir_path)-1);
        char *slash = strrchr(dir_path, '/');
        if (slash) *slash = '\0';
        
        ESP_LOGI(TAG, "MIDI: scanning directory: %s", dir_path);
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) && midi_count < 32) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && (strcasecmp(ext, ".mid") == 0 || strcasecmp(ext, ".midi") == 0)) {
                        ESP_LOGI(TAG, "MIDI: found file: %s", entry->d_name);
                        int w = snprintf(midi_playlist[midi_count], 256, "%s/%.200s", dir_path, entry->d_name);
                        if (w > 0 && w < 256) {
                            if (strstr(real_path, entry->d_name)) {
                                midi_index = midi_count;
                                ESP_LOGI(TAG, "MIDI: current track index = %d", midi_index);
                            }
                            midi_count++;
                        }
                    }
                }
            }
            closedir(dir);
            ESP_LOGI(TAG, "MIDI: total found %d midi files", midi_count);
        } else {
            ESP_LOGE(TAG, "MIDI: failed to open directory: %s", dir_path);
        }
    }
    
    /* Отправляем информацию в UI */
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    
    ui_update_t ui = {0};
    ui.hide_loading = true;
    strncpy(ui.title, fname, sizeof(ui.title)-1);
    ui.update_title = true;
    strncpy(ui.format, "MIDI", sizeof(ui.format)-1);
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    ui.update_module_info = true;
    ui.mod_channels = 2;
    ui.mod_bpm = 44100;
    ui.mod_speed = 0;
    xQueueSend(ui_update_queue, &ui, 0);
    
    
    /* Запускаем воспроизведение */
    midi_player_play(real_path);

    /* Буфер для PCM */
    #define MIDI_PCM_BUF_SIZE 512
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(MIDI_PCM_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "MIDI: failed to allocate PCM buffer");
        midi_player_stop();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    float *floatL = (float *)heap_caps_malloc(MIDI_PCM_BUF_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    float *floatR = (float *)heap_caps_malloc(MIDI_PCM_BUF_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!floatL || !floatR) {
        ESP_LOGE(TAG, "MIDI: failed to allocate float buffers");
        heap_caps_free(pcm_buf);
        midi_player_stop();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    uint32_t last_ui = 0;
    bool stream_finished = false;
    
    while (is_playing && !stream_finished) {
        vTaskDelay(pdMS_TO_TICKS(2));
        
        /* Обработка команд */
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == PLAYER_CMD_SET_VOLUME) {
                midi_player_set_volume(cmd.value / 100.0f);
                memset(&ui, 0, sizeof(ui));
                ui.volume = cmd.value;
                ui.update_volume = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
else if (cmd.type == PLAYER_CMD_NEXT_TRACK || cmd.type == PLAYER_CMD_PREV_TRACK) {
    if (midi_count > 0) {
        int next_index;
        if (repeat_mode == REPEAT_SHUFFLE) {
            next_index = esp_random() % midi_count;
        } else if (cmd.type == PLAYER_CMD_NEXT_TRACK) {
            next_index = (midi_index + 1) % midi_count;
        } else {
            next_index = (midi_index - 1 + midi_count) % midi_count;
        }
        
        ESP_LOGI(TAG, "MIDI: switching to track %d/%d: %s", next_index + 1, midi_count, midi_playlist[next_index]);
        midi_index = next_index;
        strncpy(real_path, midi_playlist[midi_index], sizeof(real_path) - 1);
        real_path[sizeof(real_path) - 1] = '\0';
        
        /* ★ ВАЖНО: Перезагружаем синтезатор с текущим SF2 ★ */
        if (midi_player_reload_for_midi(real_path, g_midi_player_sf2_path)) {
            // Обновляем UI
            const char *new_fname = strrchr(real_path, '/');
            new_fname = new_fname ? new_fname + 1 : real_path;
            memset(&ui, 0, sizeof(ui));
            strncpy(ui.title, new_fname, sizeof(ui.title)-1);
            ui.update_title = true;
            xQueueSend(ui_update_queue, &ui, 0);
            continue;
        }
    }
}
            else if (cmd.type == PLAYER_CMD_TOGGLE_REPEAT) {
                repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                memset(&ui, 0, sizeof(ui));
                ui.mode = repeat_mode;
                ui.update_mode = true;
                xQueueSend(ui_update_queue, &ui, 0);
            }
        }
        
        /* Рендерим звук */
        midi_player_render(floatL, floatR, MIDI_PCM_BUF_SIZE);
        
        /* Конвертируем float в int16 и отправляем в I2S */
        for (int i = 0; i < MIDI_PCM_BUF_SIZE; i++) {
            int32_t l = (int32_t)(floatL[i] * 32767.0f * (current_volume / 100.0f));
            int32_t r = (int32_t)(floatR[i] * 32767.0f * (current_volume / 100.0f));
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            pcm_buf[i * 2] = (int16_t)l;
            pcm_buf[i * 2 + 1] = (int16_t)r;
        }
        
        size_t bytes = MIDI_PCM_BUF_SIZE * 2 * sizeof(int16_t);
        size_t sent = 0;
        uint8_t *ptr = (uint8_t *)pcm_buf;
        while (sent < bytes && is_playing) {
            size_t wr;
            i2s_channel_write(tx_chan, ptr + sent, bytes - sent, &wr, pdMS_TO_TICKS(50));
            sent += wr;
            if (wr == 0) vTaskDelay(pdMS_TO_TICKS(2));
        }
        
        /* Проверка окончания */
if (!midi_player_is_playing()) {
    /* Автопереход на следующий трек */
    if (!stop_no_auto && midi_count > 0) {
        int next_index;
        if (repeat_mode == REPEAT_ONE) {
            next_index = midi_index;
        } else if (repeat_mode == REPEAT_SHUFFLE) {
            next_index = esp_random() % midi_count;
        } else {
            next_index = (midi_index + 1) % midi_count;
        }
        
        strncpy(real_path, midi_playlist[next_index], sizeof(real_path) - 1);
        real_path[sizeof(real_path) - 1] = '\0';
        midi_index = next_index;
        
        /* ★ Перезагружаем синтезатор ★ */
       if (midi_player_reload_for_midi(real_path, g_midi_player_sf2_path)) {
            /* Обновляем UI */
            const char *new_fname = strrchr(real_path, '/');
            new_fname = new_fname ? new_fname + 1 : real_path;
            memset(&ui, 0, sizeof(ui));
            strncpy(ui.title, new_fname, sizeof(ui.title)-1);
            ui.update_title = true;
            xQueueSend(ui_update_queue, &ui, 0);
            continue;
        }
    }
    stream_finished = true;
    break;
}
        
        /* UI обновление */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= 500) {
            last_ui = now;
            uint32_t pos = midi_player_get_position_ms();
            memset(&ui, 0, sizeof(ui));
            ui.time_ms = pos;
            ui.total_time_ms = midi_player_get_total_time_ms();
            ui.pattern = 0;
            ui.update_module_info = true;   // ← ЭТА СТРОКА ЗАСТАВЛЯЕТ ОБНОВИТЬ RAM
            ui.total_patterns = 0;
            xQueueSend(ui_update_queue, &ui, 0);
        }
    }
    
    ESP_LOGI(TAG, "MIDI task finishing, cleaning up...");
    
   
    /* Полная деинициализация MIDI плеера */
    midi_player_deinit();
    
    i2s_reset_to_default();
    
    ESP_LOGI(TAG, "MIDI task deleted");
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}

static void usb_midi_synth_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_USB_MIDI;
    const char *sf2_path = (const char *)pvParameters;
    ESP_LOGI(TAG, "USB-MIDI Synth started with SF2: %s", sf2_path);
    
    // Проверяем, что SF2 выбран
    if (strlen(g_usb_synth_sf2_path) == 0) {
        ESP_LOGE(TAG, "No SF2 selected!");
        ui_update_t ui = {0};
        snprintf(ui.title, sizeof(ui.title), "ERROR: No SF2!");
        ui.update_title = true;
        xQueueSend(ui_update_queue, &ui, 0);
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Инициализируем USB-MIDI
    if (!usb_midi_init()) {
        ESP_LOGE(TAG, "Failed to init USB-MIDI");
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Создаём парсер SF2
    SF2Parser *parser = new SF2Parser(g_usb_synth_sf2_path);
    if (!parser) {
        ESP_LOGE(TAG, "Failed to create SF2 parser");
        usb_midi_deinit();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Полная загрузка
    if (!parser->parse()) {
        ESP_LOGE(TAG, "Failed to parse SF2");
        delete parser;
        usb_midi_deinit();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Создаём синтезатор
    Synth *synth = new Synth(*parser);
    if (!synth) {
        ESP_LOGE(TAG, "Failed to create synth");
        delete parser;
        usb_midi_deinit();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Инициализируем синтезатор
    if (!synth->init()) {
        ESP_LOGE(TAG, "Failed to init synth");
        delete synth;
        delete parser;
        usb_midi_deinit();
        is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    // GM Reset
    synth->GMReset();
    
    // Отправляем информацию в UI
    ui_update_t ui = {0};
    ui.hide_loading = true;
    snprintf(ui.title, sizeof(ui.title), "USB-MIDI Synth");
    ui.update_title = true;
    snprintf(ui.format, sizeof(ui.format), "USB-MIDI");
    ui.update_format = true;
    ui.volume = current_volume;
    ui.update_volume = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    ui.update_module_info = true;
    ui.mod_channels = 2;
    ui.mod_bpm = 44100;
    ui.mod_speed = 0;
    xQueueSend(ui_update_queue, &ui, 0);
    
    ESP_LOGI(TAG, "USB-MIDI Synth ready");
    
    // Буферы для рендеринга
    #define MIDI_RENDER_BLOCK 256
    float outL[MIDI_RENDER_BLOCK];
    float outR[MIDI_RENDER_BLOCK];
    int16_t pcm_buf[MIDI_RENDER_BLOCK * 2];
    
    uint32_t last_ui_update = 0;
    
    while (is_playing) {
        // Обработка команд
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
                case PLAYER_CMD_NEXT_TRACK:
                case PLAYER_CMD_PREV_TRACK:
                    ESP_LOGI(TAG, "Exiting USB-MIDI mode");
                    is_playing = false;
                    break;
                    
case PLAYER_CMD_RELOAD_SF2:
    ESP_LOGI(TAG, "Reloading SF2: %s", g_usb_synth_sf2_path);
    
    // ★ НЕ ТРОГАЕМ USB-MIDI! ТОЛЬКО ПАРСЕР И СИНТЕЗАТОР ★
    
    // Останавливаем старый синтезатор
    if (synth) {
        delete synth;
        synth = NULL;
    }
    if (parser) {
        delete parser;
        parser = NULL;
    }
    
    // Создаём новый парсер
    parser = new SF2Parser(g_usb_synth_sf2_path);
    if (!parser) {
        ESP_LOGE(TAG, "Failed to create parser for new SF2");
        is_playing = false;
        break;
    }
    if (!parser->parse()) {
        ESP_LOGE(TAG, "Failed to parse new SF2");
        delete parser;
        parser = NULL;
        is_playing = false;
        break;
    }
    
    // Создаём новый синтезатор
    synth = new Synth(*parser);
    if (!synth) {
        ESP_LOGE(TAG, "Failed to create synth for new SF2");
        delete parser;
        parser = NULL;
        is_playing = false;
        break;
    }
    if (!synth->init()) {
        ESP_LOGE(TAG, "Failed to init synth with new SF2");
        delete synth;
        synth = NULL;
        delete parser;
        parser = NULL;
        is_playing = false;
        break;
    }
    synth->GMReset();
    ESP_LOGI(TAG, "SF2 reloaded successfully!");
    break;
                    
                case PLAYER_CMD_SET_VOLUME:
                    current_volume = cmd.value;
                    memset(&ui, 0, sizeof(ui));
                    ui.volume = cmd.value;
                    ui.update_volume = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    break;
                    
                case PLAYER_CMD_TOGGLE_REPEAT:
                    repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                    memset(&ui, 0, sizeof(ui));
                    ui.mode = repeat_mode;
                    ui.update_mode = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    break;
                    
                default:
                    break;
                        
            }
        }
        
        // Читаем MIDI из USB
        while (usb_midi_available()) {
            uint8_t packet[4];
            if (usb_midi_read(packet)) {
                uint8_t status = packet[1];
                uint8_t data1 = packet[2];
                uint8_t data2 = packet[3];
                
                if ((status & 0xF0) == 0x90 && data2 > 0) {
                    synth->noteOn(status & 0x0F, data1, data2);
                }
                else if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90) {
                    synth->noteOff(status & 0x0F, data1);
                }
                else if ((status & 0xF0) == 0xB0) {
                    synth->controlChange(status & 0x0F, data1, data2);
                }
                else if ((status & 0xF0) == 0xC0) {
                    synth->programChange(status & 0x0F, data1);
                }
                else if ((status & 0xF0) == 0xE0) {
                    int value = (data2 << 7) | data1;
                    synth->pitchBend(status & 0x0F, value);
                }
            }
        }
        
        // Рендеринг звука
        memset(outL, 0, sizeof(outL));
        memset(outR, 0, sizeof(outR));
        
        synth->renderBlock(outL, outR, MIDI_RENDER_BLOCK);
        
        // Конвертируем float в int16 для I2S
        float volume_scale = (float)current_volume / 100.0f;
        for (int i = 0; i < MIDI_RENDER_BLOCK; i++) {
            int32_t l = (int32_t)(outL[i] * 32767.0f * volume_scale);
            int32_t r = (int32_t)(outR[i] * 32767.0f * volume_scale);
            
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            
            pcm_buf[i * 2] = (int16_t)l;
            pcm_buf[i * 2 + 1] = (int16_t)r;
        }
        
        // Отправляем в I2S
        if (tx_chan) {
            size_t bytes_written;
            size_t bytes_to_write = MIDI_RENDER_BLOCK * 2 * sizeof(int16_t);
            i2s_channel_write(tx_chan, pcm_buf, bytes_to_write, &bytes_written, portMAX_DELAY);
        }
        
        // UI обновление (раз в секунду)
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui_update >= 1000) {
            last_ui_update = now;
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            int ram_kb = psram_free / 1024;
            
            memset(&ui, 0, sizeof(ui));
            ui.update_module_info = true;
            ui.mod_channels = 2;
            ui.mod_bpm = 44100;
            ui.mod_speed = ram_kb;
            xQueueSend(ui_update_queue, &ui, 0);
        }
        
        taskYIELD();
    }
    
    ESP_LOGI(TAG, "USB-MIDI Synth stopped");
    delete synth;
    delete parser;
    usb_midi_deinit();
    
    is_playing = false;

        current_player_mode = PLAYER_MODE_NONE;

    vTaskDelete(NULL);
}



// В gb_emulation_task, вместо старого вызова:
static void gb_emulation_task(void *pvParameters) {
    const char *rom_path = (const char *)pvParameters;
    ESP_LOGI(TAG, "GB emulation task STARTED for: %s", rom_path);
    
    int frame_count = 0;
    uint32_t last_fps_check = esp_timer_get_time();
    int fps_counter = 0;
    
    while (is_playing && gb_emulator_is_running()) {
        gb_emulator_run_frame();
        fps_counter++;
        frame_count++;
     /*   
        uint32_t now = esp_timer_get_time();
        if (now - last_fps_check > 1000000) {
            ESP_LOGI(TAG, "FPS: %d, frames: %d", fps_counter, frame_count);
            fps_counter = 0;
            last_fps_check = now;
        }
       */ 
        uint16_t *fb = gb_emulator_get_framebuffer();
        if (fb) {
            gb_display_adapter_update_rgb(fb);
        }
    }
    
    ESP_LOGI(TAG, "GB emulation task FINISHED, frames=%d", frame_count);
    gb_display_adapter_deinit();
    gb_emulator_deinit();
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
}




/* --- Задача вывода звука --- */
static void audio_output_task(void *pvParameters)
{
    size_t item_size; void *audio_data;
    while (is_playing) {
        
        if (!audio_ringbuf) {
    ESP_LOGE(TAG, "audio_ringbuf is NULL!");
    vTaskDelay(pdMS_TO_TICKS(100));
    continue;
}
        
        audio_data = xRingbufferReceive(audio_ringbuf, &item_size, pdMS_TO_TICKS(20));
        if (audio_data && item_size > 0) {
            ESP_LOGD(TAG, "I2S: playing %d bytes", item_size);
            size_t bytes_written;
            uint32_t t1 = esp_log_timestamp();
            i2s_channel_write(tx_chan, audio_data, item_size, &bytes_written, portMAX_DELAY);
            uint32_t t2 = esp_log_timestamp();
            ESP_LOGD(TAG, "I2S: done %d bytes in %d ms", bytes_written, t2 - t1);
            vRingbufferReturnItem(audio_ringbuf, audio_data);
        } else {
            ESP_LOGW(TAG, "I2S: no data, waiting...");
        }
    }
    vTaskDelete(NULL);
}

/* --- Задача плеера (libxmp) --- */
static void player_task(void *pvParameters)
{
    current_player_mode = PLAYER_MODE_XMP;
    vTaskDelay(pdMS_TO_TICKS(100));
    xmp_context ctx = xmp_create_context();
    if (!ctx) { vTaskDelete(NULL); return; }
    xmp_set_player(ctx, XMP_PLAYER_VOICES, 32);
    xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_LINEAR);

    const char *filepath = (const char *)pvParameters;
    char real_path[256];
    if (strncmp(filepath, "A:", 2) == 0) snprintf(real_path, sizeof(real_path), "/sdcard%s", filepath + 2);
    else strncpy(real_path, filepath, sizeof(real_path) - 1);

    char title[64] = "No Track";
    char tracker[32] = "???";

    ui_update_t ui = {0};
    ui.show_loading = true;
    xQueueSend(ui_update_queue, &ui, 0);

    if (load_new_track(ctx, real_path, title, sizeof(title), tracker, sizeof(tracker)) != ESP_OK) {
        memset(&ui, 0, sizeof(ui));
        ui.hide_loading = true;
        xQueueSend(ui_update_queue, &ui, 0);
        xmp_free_context(ctx); is_playing = false; vTaskDelete(NULL); return;
    }

        i2s_reconfigure(44100);

    struct xmp_module_info mi;
    xmp_get_module_info(ctx, &mi);
    int total_channels = mi.mod->chn;

    memset(&ui, 0, sizeof(ui));
    strncpy(ui.title, title, sizeof(ui.title) - 1);
    strncpy(ui.format, tracker, sizeof(ui.format) - 1);
    ui.update_title = true;
    ui.update_format = true;
    ui.mode = repeat_mode;
    ui.update_mode = true;
    ui.vu_total_channels = total_channels;
    ui.vu_channels = total_channels;
    ui.hide_loading = true;
    ui.update_module_info = true;
    ui.mod_channels = mi.mod->chn;
    ui.mod_patterns = mi.mod->pat;
    ui.mod_length = mi.mod->len;
    ui.mod_instruments = mi.mod->ins;
    ui.mod_samples = mi.mod->smp;
    ui.mod_speed = mi.mod->spd;
    ui.mod_bpm = mi.mod->bpm;
    xQueueSend(ui_update_queue, &ui, 0);

    struct xmp_frame_info fi;
    uint32_t total_ms = 0, last_ui_update = 0;

    while (is_playing) {
        player_cmd_t cmd;
        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
                case PLAYER_CMD_SET_VOLUME:
                    xmp_set_player(ctx, XMP_PLAYER_VOLUME, cmd.value);
                    memset(&ui, 0, sizeof(ui));
                    ui.volume = cmd.value;
                    ui.update_volume = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    break;
                case PLAYER_CMD_NEXT_TRACK:
                case PLAYER_CMD_PREV_TRACK:
                    xmp_end_player(ctx); xmp_release_module(ctx);
                    memset(&ui, 0, sizeof(ui)); ui.show_loading = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    if (playlist_count > 0) {
                        current_track_index = (cmd.type == PLAYER_CMD_NEXT_TRACK) ? 
                            ((repeat_mode == REPEAT_SHUFFLE) ? (esp_random() % playlist_count) : ((current_track_index + 1) % playlist_count)) :
                            ((repeat_mode == REPEAT_SHUFFLE) ? (esp_random() % playlist_count) : ((current_track_index - 1 + playlist_count) % playlist_count));
                        snprintf(real_path, sizeof(real_path), "%s", playlist[current_track_index]);
                        if (load_new_track(ctx, real_path, title, sizeof(title), tracker, sizeof(tracker)) == ESP_OK) {
                            xmp_get_module_info(ctx, &mi);
                            total_channels = mi.mod->chn;
                            memset(&ui, 0, sizeof(ui));
                            strncpy(ui.title, title, sizeof(ui.title) - 1);
                            strncpy(ui.format, tracker, sizeof(ui.format) - 1);
                            ui.update_title = true; ui.update_format = true;
                            ui.vu_total_channels = total_channels; ui.vu_channels = total_channels;
                            ui.hide_loading = true;
                            ui.update_module_info = true;
                            ui.mod_channels = mi.mod->chn; ui.mod_patterns = mi.mod->pat;
                            ui.mod_length = mi.mod->len; ui.mod_instruments = mi.mod->ins;
                            ui.mod_samples = mi.mod->smp; ui.mod_speed = mi.mod->spd;
                            ui.mod_bpm = mi.mod->bpm;
                            xQueueSend(ui_update_queue, &ui, 0);
                            total_ms = 0; last_ui_update = 0;
                        }
                    }
                    break;
                case PLAYER_CMD_TOGGLE_REPEAT:
                    repeat_mode = (repeat_mode_t)(((int)repeat_mode + 1) % 3);
                    memset(&ui, 0, sizeof(ui));
                    ui.mode = repeat_mode; ui.update_mode = true;
                    xQueueSend(ui_update_queue, &ui, 0);
                    break;
                        default:  // ← ДОБАВИТЬ
        break;
            }
        }

        if (is_paused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        int play_res = xmp_play_frame(ctx);
        xmp_get_frame_info(ctx, &fi);
        if (fi.total_time > 0 && fi.total_time != total_ms) total_ms = fi.total_time;
        
        if (fi.time - last_ui_update >= 50 || last_ui_update == 0) {
            memset(&ui, 0, sizeof(ui));
            ui.pattern = fi.pattern; ui.total_patterns = fi.num_rows > 0 ? fi.num_rows : 1;
            ui.row = fi.row; ui.time_ms = fi.time; ui.total_time_ms = total_ms;
            ui.vu_channels = fi.virt_used;
            xQueueSend(ui_update_queue, &ui, 0);
            last_ui_update = fi.time;
        }

        bool track_ended = (play_res == -XMP_END) || 
                          (fi.total_time > 0 && fi.time >= fi.total_time && fi.total_time > 1000) ||
                          (fi.loop_count > 0);

        if (track_ended) {
            if (repeat_mode == REPEAT_ONE) {
                xmp_end_player(ctx); xmp_release_module(ctx); vTaskDelay(pdMS_TO_TICKS(50));
                memset(&ui, 0, sizeof(ui)); ui.show_loading = true;
                xQueueSend(ui_update_queue, &ui, 0);
                if (load_new_track(ctx, real_path, title, sizeof(title), tracker, sizeof(tracker)) == ESP_OK) {
                    xmp_get_module_info(ctx, &mi); total_channels = mi.mod->chn;
                    memset(&ui, 0, sizeof(ui));
                    strncpy(ui.title, title, sizeof(ui.title) - 1);
                    strncpy(ui.format, tracker, sizeof(ui.format) - 1);
                    ui.update_title = true; ui.update_format = true;
                    ui.vu_total_channels = total_channels; ui.vu_channels = total_channels;
                    ui.hide_loading = true;
                    ui.update_module_info = true;
                    ui.mod_channels = mi.mod->chn; ui.mod_patterns = mi.mod->pat;
                    ui.mod_length = mi.mod->len; ui.mod_instruments = mi.mod->ins;
                    ui.mod_samples = mi.mod->smp; ui.mod_speed = mi.mod->spd;
                    ui.mod_bpm = mi.mod->bpm;
                    xQueueSend(ui_update_queue, &ui, 0);
                    total_ms = 0; last_ui_update = 0;
                    continue;
                }
            }
            if (playlist_count > 0 && repeat_mode != REPEAT_ONE) {
                xmp_end_player(ctx); xmp_release_module(ctx);
                current_track_index = (repeat_mode == REPEAT_SHUFFLE) ? (esp_random() % playlist_count) : ((current_track_index + 1) % playlist_count);
                snprintf(real_path, sizeof(real_path), "%s", playlist[current_track_index]);
                memset(&ui, 0, sizeof(ui)); ui.show_loading = true;
                xQueueSend(ui_update_queue, &ui, 0);
                if (load_new_track(ctx, real_path, title, sizeof(title), tracker, sizeof(tracker)) == ESP_OK) {
                    xmp_get_module_info(ctx, &mi); total_channels = mi.mod->chn;
                    memset(&ui, 0, sizeof(ui));
                    strncpy(ui.title, title, sizeof(ui.title) - 1);
                    strncpy(ui.format, tracker, sizeof(ui.format) - 1);
                    ui.update_title = true; ui.update_format = true;
                    ui.vu_total_channels = total_channels; ui.vu_channels = total_channels;
                    ui.hide_loading = true;
                    ui.update_module_info = true;
                    ui.mod_channels = mi.mod->chn; ui.mod_patterns = mi.mod->pat;
                    ui.mod_length = mi.mod->len; ui.mod_instruments = mi.mod->ins;
                    ui.mod_samples = mi.mod->smp; ui.mod_speed = mi.mod->spd;
                    ui.mod_bpm = mi.mod->bpm;
                    xQueueSend(ui_update_queue, &ui, 0);
                    total_ms = 0; last_ui_update = 0;
                    continue;
                }
            }
            is_playing = false; break;
        }

        if (fi.buffer_size > 0 && audio_ringbuf)
            xRingbufferSend(audio_ringbuf, fi.buffer, fi.buffer_size, pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    xmp_end_player(ctx); 
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    
    i2s_reset_to_default();
    current_player_mode = PLAYER_MODE_NONE;
    vTaskDelete(NULL);
    
    
}




/* --- Публичные функции --- */
esp_err_t audio_player_init(void)
{
    ui_update_queue = xQueueCreate(10, sizeof(ui_update_t));
    player_cmd_queue = xQueueCreate(5, sizeof(player_cmd_t));
    if (!ui_update_queue || !player_cmd_queue) return ESP_ERR_NO_MEM;
    return i2s_init();
}

esp_err_t audio_player_play(const char *filepath)
{
    
        ESP_LOGI(TAG, "audio_player_play: called for %s, is_playing=%d, stop_no_auto=%d", 
             filepath, is_playing, stop_no_auto);

     stop_no_auto = false;

     if (is_playing) { is_playing = false; vTaskDelay(pdMS_TO_TICKS(300)); }

if (is_gb_file(filepath)) {
    ESP_LOGI(TAG, "GameBoy ROM requested: %s", filepath);
    
    if (is_playing) {
        is_playing = false;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    
    current_player_mode = PLAYER_MODE_GB;
    
    if (gb_screen == NULL) {
        gb_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(gb_screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_pad_all(gb_screen, 0, 0);
    }
    
    lv_scr_load(gb_screen);
    input_set_mode(INPUT_MODE_PLAYER);
    
    // ★ ПЕРЕНАСТРАИВАЕМ I2S НА 32768 Гц ★
    i2s_reconfigure(32768);
    
    if (!gb_emulator_init(filepath)) {
        ESP_LOGE(TAG, "Failed to init GameBoy emulator");
        lv_scr_load(main_screen);
        input_set_mode(INPUT_MODE_FILE_EXPLORER);
        current_player_mode = PLAYER_MODE_NONE;
        return ESP_FAIL;
    }
    
    if (!gb_display_adapter_init(gb_screen)) {
        ESP_LOGE(TAG, "Failed to init display adapter");
        gb_emulator_deinit();
        lv_scr_load(main_screen);
        input_set_mode(INPUT_MODE_FILE_EXPLORER);
        current_player_mode = PLAYER_MODE_NONE;
        return ESP_FAIL;
    }
    
    is_playing = true;
    static char pc[256];
    strncpy(pc, filepath, sizeof(pc)-1);
    pc[sizeof(pc)-1] = '\0';
    
    ESP_LOGI(TAG, "Creating GB task...");
    ESP_LOGI(TAG, "Free heap before GB task: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    BaseType_t result = xTaskCreatePinnedToCore(
        gb_emulation_task, 
        "gb_emu", 
        8192,
        pc, 
        18, 
        NULL, 
        1
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GB task!");
        is_playing = false;
        gb_display_adapter_deinit();
        gb_emulator_deinit();
        lv_scr_load(main_screen);
        input_set_mode(INPUT_MODE_FILE_EXPLORER);
        current_player_mode = PLAYER_MODE_NONE;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "GB task created successfully");
    return ESP_OK;
}



    if (is_wav_file(filepath)) {
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(wav_player_task, "wav", 32768, pc, 10, NULL, 1);
        return ESP_OK;
    }

    if (is_flac_file(filepath)) {
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(flac_player_task, "flac", 32768, pc, 13, NULL, 1);
        return ESP_OK;
    }
        
    if (is_mp3_file(filepath)) {
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(mp3_player_task, "mp3", 24576, pc, 13, NULL, 1);
        return ESP_OK;
    }

    if (is_midi_file(filepath)) {
        ESP_LOGI(TAG, "AUDIO_PLAYER: Creating new MIDI task for: %s", filepath);
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(midi_player_task, "midi", 32768, pc, 13, NULL, 1);
        return ESP_OK;
    }
    
    if (is_usb_midi_file(filepath)) {
        ESP_LOGI(TAG, "USB-MIDI mode requested with SF2: %s", g_selected_sf2_path);
        is_playing = true;
        is_paused = false;
        player_stopped_for_explorer = false;
        
        static char pc[256];
        strncpy(pc, g_selected_sf2_path, sizeof(pc)-1);
        xTaskCreatePinnedToCore(usb_midi_synth_task, "usb_midi", 32768, pc, 13, NULL, 1);
        return ESP_OK;
    }
    
    if (is_opus_file(filepath)) {
        ESP_LOGI(TAG, "AUDIO_PLAYER: Creating new OPUS task for: %s", filepath);
        
        /* ★ ПРИНУДИТЕЛЬНО ПЕРЕСОЗДАЁМ I2S ДЛЯ OPUS ★ */
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_del_channel(tx_chan);
            tx_chan = NULL;
        }
        
        /* Заново инициализируем I2S */
        i2s_init();
        
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(opus_player_task, "opus", 32768, pc, 13, NULL, 1);
        return ESP_OK;
    }

        if (is_gme_file(filepath)) {
        ESP_LOGI(TAG, "AUDIO_PLAYER: Creating new GME task for: %s", filepath);
        is_playing = true; is_paused = false; player_stopped_for_explorer = false;
        static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
        xTaskCreatePinnedToCore(gme_player_task, "gme", 32768, pc, 14, NULL, 1);
        return ESP_OK;
    }

    


    scan_playlist(filepath);

    if (!audio_ringbuf) {
        audio_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (!audio_ringbuf) return ESP_ERR_NO_MEM;
    }

    is_playing = true; is_paused = false; player_stopped_for_explorer = false;
    static char pc[256]; strncpy(pc, filepath, sizeof(pc)-1);
    xTaskCreatePinnedToCore(audio_output_task, "audio_out", 8192, NULL, 15, &audio_out_task_handle, 0);
    xTaskCreatePinnedToCore(player_task, "player", 32768, pc, 11, &player_task_handle, 1);
    return ESP_OK;
}



void audio_player_process_ui(void)
{
    ui_update_t ui;
    while (xQueueReceive(ui_update_queue, &ui, 0) == pdTRUE) {
        /* ★ ОБРАБОТКА GME ИНФОРМАЦИИ (ДОБАВИТЬ ПЕРВОЙ) ★ */
        if (ui.update_gme_info) {
            ESP_LOGI(TAG, "UI: update_gme_info, game='%s', author='%s', system='%s'", 
                     ui.gme_game, ui.gme_author, ui.gme_system);
            player_ui_set_gme_info(ui.gme_game, ui.gme_author, ui.gme_system);
        }
        
        if (ui.update_module_info) {
            player_ui_set_module_info(ui.mod_channels, ui.mod_patterns, ui.mod_length, 
                                      ui.mod_instruments, ui.mod_samples, ui.mod_speed, ui.mod_bpm);
        }
        if (ui.show_loading) player_ui_show_loading(true);
        if (ui.hide_loading) player_ui_show_loading(false);
        if (ui.update_title && ui.title[0]) player_ui_set_title(ui.title);
        if (ui.update_format) player_ui_set_format(ui.format);
        if (ui.update_mode) player_ui_set_repeat_mode((repeat_mode_t)ui.mode);
        if (ui.update_volume) player_ui_set_volume(ui.volume);
        if (!ui.show_loading && !ui.hide_loading) {
            player_ui_update(ui.update_title ? ui.title : NULL, ui.pattern, ui.total_patterns, ui.row, ui.time_ms, ui.total_time_ms);
        }
    }
}

void audio_player_stop(void) { 
    stop_no_auto = true;
    is_playing = false; 
    vTaskDelay(pdMS_TO_TICKS(300)); 
    player_stopped_for_explorer = true; 
}


void audio_player_toggle_pause(void) { is_paused = !is_paused; }
void audio_player_toggle_mute(void) { if (is_muted) { current_volume = saved_volume; is_muted = false; } else { saved_volume = current_volume; current_volume = 0; is_muted = true; } player_cmd_t cmd = { .type = PLAYER_CMD_SET_VOLUME, .value = current_volume }; xQueueSend(player_cmd_queue, &cmd, 0); }
void audio_player_volume_up(void) { if (is_muted) { is_muted = false; current_volume = saved_volume; } current_volume = (current_volume + 5 > 100) ? 100 : current_volume + 5; player_cmd_t cmd = { .type = PLAYER_CMD_SET_VOLUME, .value = current_volume }; xQueueSend(player_cmd_queue, &cmd, 0); }
void audio_player_volume_down(void) { if (is_muted) { is_muted = false; current_volume = saved_volume; } current_volume = (current_volume - 5 < 0) ? 0 : current_volume - 5; player_cmd_t cmd = { .type = PLAYER_CMD_SET_VOLUME, .value = current_volume }; xQueueSend(player_cmd_queue, &cmd, 0); }
void audio_player_next_track(void) { player_cmd_t cmd = { .type = PLAYER_CMD_NEXT_TRACK }; xQueueSend(player_cmd_queue, &cmd, 0); }
void audio_player_prev_track(void) { player_cmd_t cmd = { .type = PLAYER_CMD_PREV_TRACK }; xQueueSend(player_cmd_queue, &cmd, 0); }
void audio_player_toggle_repeat(void) { player_cmd_t cmd = { .type = PLAYER_CMD_TOGGLE_REPEAT }; xQueueSend(player_cmd_queue, &cmd, 0); }

void audio_player_set_eq(eq_preset_t preset) {
    ESP_LOGI("AUDIO_PLAYER", "EQ changed from %d to %d", current_eq, preset);
    current_eq = preset;
}

eq_preset_t audio_player_get_eq(void) {
    return current_eq;
}


/* GME настройки */
gme_settings_t audio_player_get_gme_settings(void)
{
    return gme_settings;
}

void audio_player_set_gme_settings(const gme_settings_t *settings)
{
    gme_settings = *settings;
    audio_player_apply_gme_settings();
}

void audio_player_apply_gme_settings(void)
{
    if (!current_gme_emu) return;
    
    /* Применяем эквалайзер */
    gme_equalizer_t eq = { gme_settings.treble, gme_settings.bass };
    gme_set_equalizer(current_gme_emu, &eq);
    
    /* Применяем стерео глубину */
    gme_set_stereo_depth(current_gme_emu, gme_settings.stereo_depth);
    
    /* Применяем точность эмуляции */
    gme_enable_accuracy(current_gme_emu, gme_settings.accuracy);
    
    /* Применяем мьютинг голосов */
    int mute_mask = 0;
    for (int i = 0; i < 8; i++) {
        if (gme_settings.mute_voices[i]) {
            mute_mask |= (1 << i);
        }
    }
    gme_mute_voices(current_gme_emu, mute_mask);
    
    ESP_LOGI(TAG, "GME settings applied: treble=%.1f, bass=%.0f, stereo=%.2f, accuracy=%d, mute_mask=0x%02X",
             gme_settings.treble, gme_settings.bass, gme_settings.stereo_depth, 
             gme_settings.accuracy, mute_mask);
}

void audio_player_clear_ui_queue(void) { ui_update_t ui; while (xQueueReceive(ui_update_queue, &ui, 0) == pdTRUE) {} }

player_mode_t audio_player_get_current_mode(void)
{
    // ★ ИСПОЛЬЗУЕМ НОВЫЙ ЭМУЛЯТОР ★
    extern bool gb_emulator_is_running(void);
    if (gb_emulator_is_running()) {
        return PLAYER_MODE_GB;
    }
    return current_player_mode;
}



int audio_player_get_volume(void)
{
    return current_volume;
}


/* Получить путь к папке с SF2 для MIDI плеера */
const char* audio_player_get_midi_sf2_folder(void)
{
    return "/sdcard/soundfonts/MIDI_PLAYER/";
}

/* Получить путь к папке с SF2 для USB-синтезатора */
const char* audio_player_get_usb_synth_sf2_folder(void)
{
    return "/sdcard/soundfonts/USB_SYNTH/";
}

/* Получить полный путь к выбранному SF2 для MIDI плеера */
const char* audio_player_get_midi_sf2_path(void)
{
    return g_midi_player_sf2_path;
}

/* Получить полный путь к выбранному SF2 для USB-синтезатора */
const char* audio_player_get_usb_synth_sf2_path(void)
{
    return g_usb_synth_sf2_path;
}

/* Установить SF2 для MIDI плеера */
void audio_player_set_midi_sf2_path(const char *path)
{
    if (path) {
        strncpy(g_midi_player_sf2_path, path, sizeof(g_midi_player_sf2_path) - 1);
        g_midi_player_sf2_path[sizeof(g_midi_player_sf2_path) - 1] = '\0';
        ESP_LOGI(TAG, "MIDI SF2 set to: %s", g_midi_player_sf2_path);
    }
}

/* Установить SF2 для USB-синтезатора */
void audio_player_set_usb_synth_sf2_path(const char *path)
{
    if (path) {
        strncpy(g_usb_synth_sf2_path, path, sizeof(g_usb_synth_sf2_path) - 1);
        g_usb_synth_sf2_path[sizeof(g_usb_synth_sf2_path) - 1] = '\0';
        ESP_LOGI(TAG, "USB-Synth SF2 set to: %s", g_usb_synth_sf2_path);
    }
}