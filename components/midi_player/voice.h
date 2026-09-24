#ifndef MIDI_VOICE_H
#define MIDI_VOICE_H

#include <stdint.h>
#include "adsr.h"
#include "sf2_types.h"

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    SUSTAIN_LOOP = 3,
    PING_PONG_LOOP = 4
};

struct Voice {
    bool active = false;
    uint8_t channel = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
    
    // Позиция в сэмпле
    float phase = 0.0f;
    float phaseIncrement = 0.0f;
    float basePhaseIncrement = 0.0f;
    
    // Данные сэмпла
    const int16_t* data = nullptr;
    uint32_t length = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    uint32_t loopLength = 0;
    LoopType loopType = NO_LOOP;
    bool forward = true;
    
    // Параметры зоны
    sf2::Zone zone;
    
    // Громкость и панорама
    float velocityVolume = 1.0f;
    float panL = 1.0f, panR = 1.0f;
    float reverbAmount = 0.0f;
    float chorusAmount = 0.0f;
    
    // ADSR
    ADSR ampEnv;
    

    #ifdef ENABLE_IN_VOICE_FILTERS
    BiquadFilterInternalCoeffs filter;
    float filterFc = 20000.0f;
    float filterQ = 0.707f;
    #endif


    // Оценка для алгоритма вытеснения
    float score = 0.0f;
    uint32_t exclusiveClass = 0;
    bool noteHeld = true;
    
    // Методы
    
    void init(int blockSize = 1);
    void start(uint8_t ch, uint8_t note, uint8_t vel, const sf2::Zone& z,
               float pitchBendFactor, float channelVolume, float channelExpression, float channelPan);
    void stop();
    void kill();
    void updatePitch(float pitchBendFactor);
    void updatePan();
    void updateVolume(float channelVolume, float channelExpression);
    void updateScore();
    float nextSample();
    bool isRunning() const;
    sf2::SampleHeader* sample = nullptr;
    float* modPan = nullptr;   // указатель на панораму канала
private:
    void updateLoop();
};

#endif // MIDI_VOICE_H