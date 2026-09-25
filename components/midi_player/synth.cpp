#include "synth.h"
#include "esp_log.h"
#include <cmath>
#include <cstring>

#include "config.h"
static const char* TAG = "Synth";

// Вспомогательные функции
static inline float soft_clip(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

Synth::Synth(SF2Parser& parser) : _parser(parser), _volumeScaler(0.65f), _sf2Loaded(false) {
    for (int i = 0; i < MAX_VOICES; i++) {
        _voices[i].init(1);  // blockSize = 1
    }
    for (int i = 0; i < 16; i++) {
        _channels[i].reset();
    }
}

Synth::~Synth() {}

bool Synth::init() {
    if (!_parser.parse()) {
        ESP_LOGE(TAG, "Failed to parse SF2");
        return false;
    }
    _sf2Loaded = true;
    GMReset();
    return true;
}

void Synth::reset() {
    for (int i = 0; i < MAX_VOICES; i++) {
        _voices[i].kill();
    }
}

void Synth::GMReset() {
    for (int ch = 0; ch < 16; ch++) {
        _channels[ch].reset();
        _channels[ch].wantProgram = 0;
        _channels[ch].wantBankMSB = (ch == 9) ? 128 : 0;
        _channels[ch].wantBankLSB = 0;
        applyBankProgram(ch);
    }
    reset();
}

void Synth::applyBankProgram(uint8_t channel) {
    auto& state = _channels[channel];
    uint8_t program = state.wantProgram;
    uint16_t bank = (state.wantBankMSB << 7) | state.wantBankLSB;
    
    // Определяем, является ли канал ударным (канал 9 или специальный банк)
    if (channel == 9 || state.wantBankMSB == 127 || state.wantBankMSB == 120 || bank == 128) {
        state.isDrum = true;
    } else {
        state.isDrum = false;
    }
    
    // Проверяем наличие пресета
    if (_parser.hasPreset(bank, program)) {
        state.program = program;
        state.setBank(bank);
        ESP_LOGD(TAG, "Ch%d: Program=%d, Bank=%d %s", 
                 channel + 1, program, bank, state.isDrum ? "(Drum)" : "");
    } else {
        // Fallback на программу 0
        uint16_t fallbackBank = state.isDrum ? 128 : 0;
        if (_parser.hasPreset(fallbackBank, 0)) {
            state.program = 0;
            state.setBank(fallbackBank);
            ESP_LOGW(TAG, "Ch%d: Fallback to Program=0, Bank=%d", channel + 1, fallbackBank);
        }
    }
}

std::vector<sf2::Zone> Synth::getZonesForNote(uint8_t channel, uint8_t note, uint8_t velocity,
                                               uint16_t bank, uint16_t program) {
    return _parser.getZonesForNote(channel, note, velocity, bank, program);
}

void Synth::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) {
        noteOff(channel, note);
        return;
    }
    
    auto& state = _channels[channel];
    state.activityIncrease(velocity);
    
    auto zones = getZonesForNote(channel, note, velocity, state.getBank(), state.program);
    if (zones.empty()) return;
    
    state.pushNote(note);
    
    // Моно-режим
    bool isMono = state.monoMode != ChannelState::Poly;
    bool retrig = state.monoMode != ChannelState::MonoLegato;
    
    if (isMono) {
        if (retrig) {
            // Убиваем все текущие голоса на этом канале
            for (auto& v : _voices) {
                if (v.active && v.channel == channel) {
                    v.kill();
                }
            }
            // Запускаем новые голоса
            for (auto& zone : zones) {
                float score = (float)velocity / 127.0f;
                Voice* v = allocateVoice(channel, note, score, zone.exclusiveClass);
                if (v) {
                    v->start(channel, note, velocity, zone, 
                             state.pitchBendFactor, state.volume, state.expression, state.pan);
                }
            }
        } else {
            // Legato: обновляем pitch существующих голосов
            bool reused = false;
            for (auto& v : _voices) {
                if (v.active && v.channel == channel) {
                    if (!v.noteHeld) {
                        v.kill();
                    } else {
                        v.updatePitch(state.pitchBendFactor);
                        reused = true;
                    }
                }
            }
            if (!reused) {
                for (auto& zone : zones) {
                    float score = (float)velocity / 127.0f;
                    Voice* v = allocateVoice(channel, note, score, zone.exclusiveClass);
                    if (v) {
                        v->start(channel, note, velocity, zone,
                                 state.pitchBendFactor, state.volume, state.expression, state.pan);
                    }
                }
            }
        }
    } else {
        // Поли-режим
        for (auto& zone : zones) {
            float score = (float)velocity / 127.0f;
            Voice* v = allocateVoice(channel, note, score, zone.exclusiveClass);
            if (v) {
                v->start(channel, note, velocity, zone,
                         state.pitchBendFactor, state.volume, state.expression, state.pan);
            }
        }
    }
}

void Synth::noteOff(uint8_t channel, uint8_t note) {
    auto& state = _channels[channel];
    bool isMono = state.monoMode != ChannelState::Poly;
    
    state.removeNote(note);
    
    int found = 0;
    for (auto& v : _voices) {
        if (!v.active || v.channel != channel) continue;
        
        if (isMono) {
            if (!state.hasNotes()) {
                v.stop();
                found++;
            }
        } else {
            if (v.note == note) {
                v.stop();
                found++;
            }
        }
    }
    
}

void Synth::controlChange(uint8_t channel, uint8_t control, uint8_t value) {
    auto& state = _channels[channel];
    float fval = (float)value / 127.0f;
    
    switch (control) {
        case 0:   // Bank Select MSB
            state.wantBankMSB = value & 0x7F;
            break;
        case 32:  // Bank Select LSB
            state.wantBankLSB = value & 0x7F;
            break;
        case 1:   // Mod Wheel
            state.modWheel = fval;
            break;
        case 5:   // Portamento Time
            state.portaTime = fval;
            break;
        case 7:   // Channel Volume
            state.volume = fval;
            break;
        case 10:  // Pan
            state.pan = fval;
            for (auto& v : _voices) {
                if (v.active && v.channel == channel) {
                v.updatePan();  // ← обновляем панораму для уже играющих голосов
        }
    }
    break;
        case 11:  // Expression
            state.expression = fval;
            break;
        case 64:  // Sustain Pedal
            state.sustainPedal = (value >= 64);
            if (!state.sustainPedal) {
                for (auto& v : _voices) {
                    if (v.active && v.channel == channel && !v.noteHeld) {
                        v.stop();
                    }
                }
            }
            break;
        case 65:  // Portamento
            state.portamento = (value >= 64);
            break;
        case 91:  // Reverb Send
            state.reverbSend = fval;
            break;
        case 93:  // Chorus Send
            state.chorusSend = fval;
            break;
        case 120: // All Sound Off
            for (auto& v : _voices) {
                if (v.active && v.channel == channel) v.kill();
            }
            break;
        case 123: // All Notes Off
            for (auto& v : _voices) {
                if (v.active && v.channel == channel) v.stop();
            }
            break;
        case 126: // Mono Mode
            state.monoMode = (value > 0) ? ChannelState::MonoLegato : ChannelState::MonoRetrig;
            break;
        case 127: // Poly Mode
            state.monoMode = ChannelState::Poly;
            break;
        default:
            break;
    }
}

void Synth::programChange(uint8_t channel, uint8_t program) {
    auto& state = _channels[channel];
    state.wantProgram = program & 0x7F;
    applyBankProgram(channel);
}

void Synth::pitchBend(uint8_t channel, int value) {
    auto& state = _channels[channel];
    float norm = (value - PITCH_BEND_CENTER) / 8192.0f;
    float semis = norm * state.pitchBendRange;
    state.pitchBend = norm;
    state.pitchBendFactor = exp2f(semis / 12.0f);
    
    for (auto& v : _voices) {
        if (v.active && v.channel == channel) {
            v.updatePitch(state.pitchBendFactor);
        }
    }
}

Voice* Synth::allocateVoice(uint8_t channel, uint8_t note, float score, uint32_t exclusiveClass) {
    Voice* v = findWeakestVoice(channel, note, score, exclusiveClass);
    if (!v) v = findWorstVoice();
    return v;
}

Voice* Synth::findWeakestVoice(uint8_t channel, uint8_t note, float score, uint32_t exclusiveClass) {
    Voice* weakest = nullptr;
    float weakestScore = 1e9f;
    
    for (int i = 0; i < MAX_VOICES; i++) {
        auto& v = _voices[i];
        if (v.active && v.channel == channel) {
            if (exclusiveClass > 0 && v.exclusiveClass == exclusiveClass) {
                v.kill();
                return &v;
            }
            if (v.note == note) {
                v.updateScore();
                if (v.score < weakestScore) {
                    weakestScore = v.score;
                    weakest = &v;
                }
            }
        }
    }
    return weakest;
}

Voice* Synth::findWorstVoice() {
    Voice* worst = nullptr;
    float minScore = 1e9f;
    
    for (int i = 0; i < MAX_VOICES; i++) {
        _voices[i].updateScore();
        if (!_voices[i].active || !_voices[i].isRunning()) {
            return &_voices[i];
        }
        if (_voices[i].score < minScore) {
            minScore = _voices[i].score;
            worst = &_voices[i];
        }
    }
    return worst;
}

void Synth::renderBlock(float* outL, float* outR, int samples) {
    memset(outL, 0, samples * sizeof(float));
    memset(outR, 0, samples * sizeof(float));
    
    // Временно убираем эффекты для простоты
     float fxL[samples];
     float fxR[samples];
     memset(fxL, 0, samples * sizeof(float));
     memset(fxR, 0, samples * sizeof(float));
    
    int active_voices = 0;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (_voices[v].active) active_voices++;
    }
    
    for (int v = 0; v < MAX_VOICES; v++) {
        auto& voice = _voices[v];
        if (!voice.active) continue;
        
        float volL = _volumeScaler * voice.panL;
        float volR = _volumeScaler * voice.panR;
        
        for (int i = 0; i < samples; i++) {
            float smp = voice.nextSample();
            if (smp != 0.0f) {
                ESP_LOGD(TAG, "voice %d: smp=%f", v, smp);
            }
            outL[i] += smp * volL;
            outR[i] += smp * volR;
        }
    }
    
    // Soft clipping
    for (int i = 0; i < samples; i++) {
        outL[i] = soft_clip(outL[i]);
        outR[i] = soft_clip(outR[i]);
        
        if (outL[i] > 0.99f) outL[i] = 0.99f;
        if (outL[i] < -0.99f) outL[i] = -0.99f;
        if (outR[i] > 0.99f) outR[i] = 0.99f;
        if (outR[i] < -0.99f) outR[i] = -0.99f;
    }
}