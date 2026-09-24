#include "voice.h"
#include <math.h>
#include "config.h"
#include "esp_log.h"

#define ONE_DIV_32768 (1.0f / 32768.0f)
#define DIV_12 (1.0f / 12.0f)
#define DIV_SAMPLE_RATE (1.0f / (float)SAMPLE_RATE)

void Voice::init(int blockSize) {
    active = false;
    ampEnv.init(SAMPLE_RATE, blockSize);
}

void Voice::start(uint8_t ch, uint8_t note, uint8_t vel, const sf2::Zone& z,
                  float pitchBendFactor, float channelVolume, float channelExpression, float channelPan)
{
    zone = z;
    this->channel = ch;
    this->note = note;
    velocity = vel;
    sample = zone.sample;
    
    if (!sample || !sample->data) {
        active = false;
        return;
    }
    
    data = sample->data;
    length = sample->end - sample->start;
    
    // Настройка лупа
    int32_t loopStartOffset = zone.loopStartOffset + (zone.loopStartCoarseOffset << 15);
    int32_t loopEndOffset = zone.loopEndOffset + (zone.loopEndCoarseOffset << 15);
    loopStart = sample->startLoop + loopStartOffset - sample->start;
    loopEnd = sample->endLoop + loopEndOffset - sample->start;
    loopLength = loopEnd - loopStart;
    
    uint32_t sampleModes = zone.sampleModes & 0x0003;
    if (sampleModes == 0) loopType = NO_LOOP;
    else if (sampleModes == 1) loopType = FORWARD_LOOP;
    else if (sampleModes == 3) loopType = SUSTAIN_LOOP;
    else loopType = NO_LOOP;
    
    // Начальная позиция
    phase = 0.0f;
    forward = true;
    
    // Расчёт скорости воспроизведения
    int rootKey = (zone.rootKey >= 0) ? zone.rootKey : sample->originalPitch;
    float semi = (note - rootKey) + (sample->pitchCorrection * 0.01f) + zone.coarseTune + zone.fineTune;
    float noteRatio = exp2f(semi * DIV_12);
    float sampleRateRatio = (float)sample->sampleRate * DIV_SAMPLE_RATE;
    basePhaseIncrement = sampleRateRatio * noteRatio;
    phaseIncrement = basePhaseIncrement * pitchBendFactor;
    
    // Громкость
    float attenDb = zone.attenuation * 0.1f;
    float attenuation = exp2f(attenDb / 6.0f);
    velocityVolume = ((float)vel / 127.0f) * attenuation * channelVolume * channelExpression;
    
    // Сохраняем указатели для динамического обновления
    modPan = &channelPan;
    
    // Панорама
    updatePan();
    
    // Эффекты
    reverbAmount = zone.reverbSend;
    chorusAmount = zone.chorusSend;
    
    // ADSR
    ampEnv.setAttackTime(zone.attackTime);
    ampEnv.setHoldTime(zone.holdTime);
    ampEnv.setDecayTime(zone.decayTime);
    ampEnv.setSustainLevel(zone.sustainLevel);
    ampEnv.setReleaseTime(zone.releaseTime);
    
    ampEnv.retrigger(ADSR::END_NOW);
    
    exclusiveClass = zone.exclusiveClass;
    noteHeld = true;
    active = true;
    score = 1.0f;
}

void Voice::stop() {
    noteHeld = false;
    ampEnv.release(ADSR::END_REGULAR);
}

void Voice::kill() {
    ampEnv.release(ADSR::END_NOW);
    active = false;
}

void Voice::updatePitch(float pitchBendFactor) {
    phaseIncrement = basePhaseIncrement * pitchBendFactor;
}

void Voice::updatePan() {
    float pZone = zone.pan;
    float pMod = modPan ? (*modPan * 2.0f - 1.0f) : 0.0f;
    float p = pZone + pMod;
    if (p < -1.0f) p = -1.0f;
    if (p > 1.0f) p = 1.0f;
    p = 0.5f * (p + 1.0f);
    panL = 1.0f - p;
    panR = p;
}

void Voice::updateVolume(float channelVolume, float channelExpression) {
    // Громкость уже учтена в velocityVolume
}

void Voice::updateScore() {
    if (!active) {
        score = 0.0f;
        return;
    }
    float env = ampEnv.getValue();
    score = env * velocityVolume;
    if (!ampEnv.isActive()) {
        score *= 0.1f;
    }
}

float Voice::nextSample() {
    if (!active || !data) return 0.0f;
    
    // Интерполяция
    uint32_t idx = (uint32_t)phase;
    if (idx >= length) {
        active = false;
        return 0.0f;
    }
    
    float frac = phase - (float)idx;
    int16_t s0 = data[idx];
    int16_t s1 = (idx + 1 < length) ? data[idx + 1] : 0;
    float sample = (s0 + (s1 - s0) * frac) * ONE_DIV_32768;
    
    // Огибающая
    float env = ampEnv.process();
    
    // Выход
    float out = sample * velocityVolume * env;
    
    // Обновление фазы с учётом лупа
    phase += phaseIncrement;
    updateLoop();
    
    // Проверка активности
    if (ampEnv.isIdle()) {
        active = false;
    }
    
    return out;
}

void Voice::updateLoop() {
    switch (loopType) {
        case FORWARD_LOOP:
            while (phase >= loopEnd) {
                phase -= loopLength;
            }
            break;
            
        case SUSTAIN_LOOP:
            if (noteHeld) {
                while (phase >= loopEnd) {
                    phase -= loopLength;
                }
            } else {
                loopType = NO_LOOP;
            }
            break;
            
        case PING_PONG_LOOP:
            if (forward) {
                if (phase >= loopEnd) {
                    phase = 2.0f * loopEnd - phase;
                    forward = false;
                }
            } else {
                if (phase <= loopStart) {
                    phase = 2.0f * loopStart - phase;
                    forward = true;
                }
            }
            break;
            
        default:
            if (phase >= length) {
                active = false;
            }
            break;
    }
}

bool Voice::isRunning() const {
    return active && ampEnv.isActive();
}