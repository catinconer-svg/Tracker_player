#ifndef SF2_TYPES_H
#define SF2_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string>

namespace sf2 {

// Генераторы (из operators.h)
enum class GeneratorOperator : uint16_t {
    StartAddrOffset            = 0,
    EndAddrOffset              = 1,
    StartLoopAddrOffset        = 2,
    EndLoopAddrOffset          = 3,
    StartAddrCoarseOffset      = 4,
    ModLfoToPitch              = 5,
    VibLfoToPitch              = 6,
    ModEnvToPitch              = 7,
    InitialFilterFc            = 8,
    InitialFilterQ             = 9,
    ModLfoToFilterFc           = 10,
    ModEnvToFilterFc           = 11,
    EndAddrCoarseOffset        = 12,
    ModLfoToVolume             = 13,
    ChorusEffectsSend          = 15,
    ReverbEffectsSend          = 16,
    Pan                        = 17,
    ModLfoDelay                = 21,
    ModLfoFreq                 = 22,
    VibLfoDelay                = 23,
    VibLfoFreq                 = 24,
    DelayModEnv                = 25,
    AttackModEnv               = 26,
    HoldModEnv                 = 27,
    DecayModEnv                = 28,
    SustainModEnv              = 29,
    ReleaseModEnv              = 30,
    KeynumToModEnvHold         = 31,
    KeynumToModEnvDecay        = 32,
    DelayVolEnv                = 33,
    AttackVolEnv               = 34,
    HoldVolEnv                 = 35,
    DecayVolEnv                = 36,
    SustainVolEnv              = 37,
    ReleaseVolEnv              = 38,
    KeynumToVolEnvHold         = 39,
    KeynumToVolEnvDecay        = 40,
    Instrument                 = 41,
    KeyRange                   = 43,
    VelRange                   = 44,
    StartLoopAddrCoarseOffset  = 45,
    Keynum                     = 46,
    Velocity                   = 47,
    InitialAttenuation         = 48,
    EndLoopAddrCoarseOffset    = 50,
    CoarseTune                 = 51,
    FineTune                   = 52,
    SampleID                   = 53,
    SampleModes                = 54,
    ScaleTuning                = 56,
    ExclusiveClass             = 57,
    OverridingRootKey          = 58,
};

// Генератор с union для разных типов данных
struct Generator {
    uint16_t oper;
    union {
        struct { uint8_t lo; uint8_t hi; } range;
        uint16_t uAmount;
        int16_t sAmount;
    } amount;
};

// Заголовок сэмпла (46 байт)
#pragma pack(push, 1)
struct SampleHeader {
    char name[20];
    uint32_t start;
    uint32_t end;
    uint32_t startLoop;
    uint32_t endLoop;
    uint32_t sampleRate;
    uint8_t originalPitch;
    int8_t pitchCorrection;
    uint16_t sampleLink;
    uint16_t sampleType;
    
    int16_t* data;      // PCM данные (выделяются отдельно)
    size_t dataSize;    // Размер в байтах
};
#pragma pack(pop)

// Зона (регион) инструмента
struct Zone {
    uint8_t velLo = 0;
    uint8_t velHi = 127;
    uint8_t keyLo = 0;
    uint8_t keyHi = 127;
    SampleHeader* sample = nullptr;
    
    // Параметры из SF2
    int rootKey = -1;
    int sampleModes = 0;
    int exclusiveClass = 0;
    float fineTune = 0.0f;
    float coarseTune = 0.0f;
    
    // ADSR
    float attackTime = 0.0f;
    float holdTime = 0.0f;
    float decayTime = 0.0f;
    float sustainLevel = 1.0f;
    float releaseTime = 0.0f;
    
    // Панорама
    float pan = 0.0f;
    
    // Ослабление
    float attenuation = 1.0f;
    
    // Эффекты
    float reverbSend = 0.0f;
    float chorusSend = 0.0f;
    
    // Луп
    int32_t loopStartOffset = 0;
    int32_t loopEndOffset = 0;
    int32_t loopStartCoarseOffset = 0;
    int32_t loopEndCoarseOffset = 0;
    
    // Фильтры (опционально)
    float filterFc = 20000.0f;
    float filterQ = 0.707f;
};

} // namespace sf2

#endif // SF2_TYPES_H