#include "midi_player.h"
#include "tml.h"
#include "sf2_parser.h"
#include "synth.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "MIDI_PLAYER";

/* Глобальные объекты */
SF2Parser *g_parser = nullptr;
Synth *g_synth = nullptr;
tml_message *g_midi_data = nullptr;
tml_message *g_current_msg = nullptr;
bool g_is_playing = false;
bool g_is_paused = false;
float g_volume = 0.8f;
unsigned int g_current_time_ms = 0;
unsigned int g_total_time_ms = 0;

/* Буфер для рендеринга */
#define MIDI_RENDER_BLOCK 256

/* ---- Анализ использованных программ ---- */
int midi_get_program_usage(const char *midi_path, midi_program_usage_t *usage, int max_count)
{
    if (!usage || max_count <= 0) return 0;
    
    tml_message *midi = tml_load_filename(midi_path);
    if (!midi) return 0;
    
    int count = 0;
    tml_message *msg = midi;
    
    while (msg && count < max_count) {
        if (msg->type == TML_PROGRAM_CHANGE) {
            int channel = msg->channel;
            uint16_t bank = (channel == 9) ? 128 : 0;
            
            /* Проверяем, не использовали ли уже эту программу */
            bool found = false;
            for (int i = 0; i < count; i++) {
                if (usage[i].bank == bank && usage[i].program == msg->program) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                usage[count].bank = bank;
                usage[count].program = msg->program;
                usage[count].channel = channel;
                count++;
            }
        }
        msg = msg->next;
    }
    
    tml_free(midi);
    return count;
}

/* Перезагрузка синтезатора для нового MIDI файла */
bool midi_player_reload_for_midi(const char *midi_path, const char *sf2_path)
{
    if (!g_parser || !g_synth) {
        ESP_LOGE(TAG, "MIDI player not initialized");
        return false;
    }
    
    // 1. Останавливаем текущее воспроизведение
    midi_player_stop();
    
    // 2. Очищаем старые сэмплы
    g_parser->clear();
    
    // 3. ★ ЗАГРУЖАЕМ SF2 ЗАНОВО (если путь изменился) ★
    // Проверяем, изменился ли SF2
    static char last_sf2_path[256] = "";
    if (strcmp(last_sf2_path, sf2_path) != 0) {
        ESP_LOGI(TAG, "SF2 changed, reloading: %s", sf2_path);
        strncpy(last_sf2_path, sf2_path, sizeof(last_sf2_path) - 1);
        
        // Пересоздаём парсер с новым SF2
        delete g_parser;
        g_parser = new SF2Parser(sf2_path);
        if (!g_parser) {
            ESP_LOGE(TAG, "Failed to create SF2 parser");
            return false;
        }
        
        // Анализируем MIDI файл для загрузки только нужных сэмплов
        midi_program_usage_t usage[32];
        int usage_count = midi_get_program_usage(midi_path, usage, 32);
        
        if (usage_count > 0) {
            uint16_t banks[32], programs[32];
            for (int i = 0; i < usage_count; i++) {
                banks[i] = usage[i].bank;
                programs[i] = usage[i].program;
            }
            if (!g_parser->parse_lazy(banks, programs, usage_count)) {
                ESP_LOGE(TAG, "Failed to parse SF2 with lazy loading");
                return false;
            }
        } else {
            if (!g_parser->parse()) {
                ESP_LOGE(TAG, "Failed to parse SF2");
                return false;
            }
        }
        
        // Пересоздаём синтезатор
        delete g_synth;
        g_synth = new Synth(*g_parser);
        if (!g_synth || !g_synth->init()) {
            ESP_LOGE(TAG, "Failed to create synth");
            return false;
        }
    } else {
        // SF2 не изменился, просто перезагружаем сэмплы для нового MIDI
        ESP_LOGI(TAG, "SF2 same, reloading samples for new MIDI");
        // Очищаем старые сэмплы из парсера
        g_parser->clear();
        
        // Анализируем новый MIDI файл
        midi_program_usage_t usage[32];
        int usage_count = midi_get_program_usage(midi_path, usage, 32);
        
        if (usage_count > 0) {
            uint16_t banks[32], programs[32];
            for (int i = 0; i < usage_count; i++) {
                banks[i] = usage[i].bank;
                programs[i] = usage[i].program;
            }
            if (!g_parser->parse_lazy(banks, programs, usage_count)) {
                ESP_LOGE(TAG, "Failed to parse SF2 with lazy loading");
                return false;
            }
        } else {
            if (!g_parser->parse()) {
                ESP_LOGE(TAG, "Failed to parse SF2");
                return false;
            }
        }
    }
    
    // 4. Переинициализируем синтезатор
    g_synth->GMReset();
    
    // 5. Загружаем новый MIDI файл
    midi_player_play(midi_path);
    
    return true;
}




/* ---- Инициализация ---- */
bool midi_player_init(const char *sf2_path)
{
    ESP_LOGI(TAG, "Initializing MIDI player with SF2: %s", sf2_path);
    
    if (g_parser && g_synth) {
        ESP_LOGI(TAG, "MIDI player already initialized");
        return true;
    }
    
    g_parser = new SF2Parser(sf2_path);
    if (!g_parser) {
        ESP_LOGE(TAG, "Failed to create SF2Parser");
        return false;
    }
    
    g_synth = new Synth(*g_parser);
    if (!g_synth) {
        ESP_LOGE(TAG, "Failed to create Synth");
        delete g_parser;
        g_parser = nullptr;
        return false;
    }
    
    ESP_LOGI(TAG, "MIDI player objects created");
    return true;
}

/* ---- Деинициализация ---- */
void midi_player_deinit(void)
{
    ESP_LOGI(TAG, "MIDI player deinitializing");
    
    /* ★ ДОБАВЛЯЕМ ПРОВЕРКУ: освобождаем только если данные есть */
    if (g_midi_data) {
        heap_caps_free(g_midi_data);
        g_midi_data = nullptr;
        ESP_LOGI(TAG, "MIDI data freed");
    }
    
    if (g_synth) {
        delete g_synth;
        g_synth = nullptr;
        ESP_LOGI(TAG, "Synth deleted");
    }
    
    if (g_parser) {
        delete g_parser;
        g_parser = nullptr;
        ESP_LOGI(TAG, "Parser deleted");
    }
    
    g_current_msg = nullptr;
    g_is_playing = false;
    g_is_paused = false;
    g_current_time_ms = 0;
    g_total_time_ms = 0;
}

/* ---- Обработка MIDI событий ---- */
static void process_midi_events(unsigned int current_time)
{
    if (!g_synth || !g_current_msg) return;
    
    while (g_current_msg && g_current_msg->time <= current_time) {
        tml_message *msg = g_current_msg;
        
        switch (msg->type) {
            case TML_NOTE_ON:
                if (msg->velocity > 0) {
                    g_synth->noteOn(msg->channel, msg->key, msg->velocity);
                } else {
                    if (msg->channel != 9) {
                        g_synth->noteOff(msg->channel, msg->key);
                    }
                }
                break;
                
            case TML_NOTE_OFF:
                if (msg->channel != 9) {
                    g_synth->noteOff(msg->channel, msg->key);
                }
                break;
                
            case TML_CONTROL_CHANGE:
                g_synth->controlChange(msg->channel, msg->control, msg->control_value);
                break;
                
            case TML_PROGRAM_CHANGE:
                g_synth->programChange(msg->channel, msg->program);
                break;
                
            case TML_PITCH_BEND:
                g_synth->pitchBend(msg->channel, msg->pitch_bend);
                break;
                
            default:
                break;
        }
        
        g_current_msg = msg->next;
    }
}

/* ---- Рендеринг ---- */
void midi_player_render(float *outL, float *outR, int samples)
{
    if (!g_is_playing || g_is_paused || !g_synth) {
        if (outL && outR) {
            for (int i = 0; i < samples; i++) {
                outL[i] = 0;
                outR[i] = 0;
            }
        }
        return;
    }
    
    int samples_rendered = 0;
    
    while (samples_rendered < samples) {
        int block = MIDI_RENDER_BLOCK;
        if (block > samples - samples_rendered) {
            block = samples - samples_rendered;
        }
        
        g_synth->renderBlock(outL + samples_rendered, outR + samples_rendered, block);
        
        samples_rendered += block;
        
        g_current_time_ms += (block * 1000) / 44100;
        process_midi_events(g_current_time_ms);
        
        if (!g_current_msg && g_current_time_ms >= g_total_time_ms) {
            g_is_playing = false;
            break;
        }
    }
}

/* ---- Воспроизведение ---- */
void midi_player_play(const char *midi_path)
{
    ESP_LOGI(TAG, "Playing MIDI: %s", midi_path);
    
    /* ★ ПРОСТО И БЕЗОПАСНО: освобождаем старые данные */
    if (g_midi_data) {
        // НЕПОСРЕДСТВЕННО перед освобождением проверяем, что указатель не NULL
        // и освобождаем только если он действительно был выделен
        heap_caps_free(g_midi_data);
        g_midi_data = nullptr;
        ESP_LOGI(TAG, "Old MIDI data freed");
    }
    
    /* Загружаем MIDI файл */
    g_midi_data = tml_load_filename(midi_path);
    if (!g_midi_data) {
        ESP_LOGE(TAG, "Failed to load MIDI file: %s", midi_path);
        return;
    }
    
    /* ★ ДОБАВЛЯЕМ: получаем информацию о файле */
    int used_channels, used_programs, total_notes;
    unsigned int time_first_note;
    tml_get_info(g_midi_data, &used_channels, &used_programs, &total_notes, &time_first_note, &g_total_time_ms);
    
    ESP_LOGI(TAG, "MIDI info: channels=%d, programs=%d, notes=%d, length=%u ms",
             used_channels, used_programs, total_notes, g_total_time_ms);
    
    /* Сбрасываем синтезатор */
    if (g_synth) {
        g_synth->GMReset();
    }
    
    /* Запускаем воспроизведение */
    g_current_msg = g_midi_data;
    g_current_time_ms = 0;
    g_is_playing = true;
    g_is_paused = false;
}

/* ---- Остановка ---- */
void midi_player_stop(void)
{
    ESP_LOGI(TAG, "midi_player_stop called");
    g_is_playing = false;
    g_is_paused = false;
    g_current_time_ms = 0;
    g_current_msg = nullptr;
    
    /* ★ ДОБАВЛЯЕМ: останавливаем все звучащие ноты */
    if (g_synth) {
        g_synth->reset();
        ESP_LOGI(TAG, "Synth reset");
    }
    
    /* ★ НЕ ОСВОБОЖДАЕМ g_midi_data ЗДЕСЬ - только в midi_player_play */
    ESP_LOGI(TAG, "MIDI stopped (data kept for reload)");
}

/* ---- Статус ---- */
bool midi_player_is_playing(void)
{
    return g_is_playing;
}

bool midi_player_is_paused(void)
{
    return g_is_paused;
}

/* ---- Громкость ---- */
void midi_player_set_volume(float volume)
{
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    g_volume = volume;
    ESP_LOGI(TAG, "MIDI volume set to %.2f", g_volume);
}

/* ---- Пауза ---- */
void midi_player_pause(bool pause)
{
    g_is_paused = pause;
    ESP_LOGI(TAG, "MIDI pause: %s", pause ? "ON" : "OFF");
}

/* ---- Получение времени ---- */
unsigned int midi_player_get_total_time_ms(void)
{
    return g_total_time_ms;
}

unsigned int midi_player_get_position_ms(void)
{
    return g_current_time_ms;
}

/* ---- Получение информации о MIDI файле ---- */
bool midi_player_get_info(const char *midi_path, midi_info_t *info)
{
    if (!info) return false;
    
    tml_message *midi = tml_load_filename(midi_path);
    if (!midi) return false;
    
    unsigned int time_first_note;
    tml_get_info(midi, &info->used_channels, &info->used_programs, 
                 &info->total_notes, &time_first_note, &info->time_length_ms);
    info->time_first_note_ms = time_first_note;
    
    tml_free(midi);
    return true;
}