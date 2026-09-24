#ifndef MIDI_CHANNEL_H
#define MIDI_CHANNEL_H

#include <stdint.h>
#include <array>

struct ChannelState {
    // Основные параметры
    bool isDrum = false;
    float volume = 1.0f;      // CC#7
    float expression = 1.0f;  // CC#11
    float pan = 0.5f;         // 0=левый, 1=правый
    float modWheel = 0.0f;    // CC#1
    
    // Pitch bend
    float pitchBend = 0.0f;
    float pitchBendRange = 2.0f;
    float pitchBendFactor = 1.0f;
    
    // Педали
    uint32_t sustainPedal = 0;
    uint32_t portamento = 0;
    float portaTime = 0.2f;
    
    // Банк и программа
    uint32_t bankMSB = 0;
    uint32_t bankLSB = 0;
    uint32_t program = 0;
    uint32_t wantBankMSB = 0;
    uint32_t wantBankLSB = 0;
    uint32_t wantProgram = 0;
    
    // Тюнинг
    float tuningSemitones = 0.0f;
    
    // Модификаторы ADSR
    float attackModifier = 1.0f;
    float releaseModifier = 1.0f;
    
    // Стек нот (для моно-режима)
    std::array<uint8_t, 8> noteStack = {};
    uint8_t stackSize = 0;
    
    // Моно/поли режим
    enum MonoMode : uint8_t { Poly = 0, MonoLegato = 1, MonoRetrig = 2 };
    MonoMode monoMode = Poly;
    
    // Активность канала (для UI)
    float activity = 0.0f;
    
    // Отправка эффектов
    float reverbSend = 0.0f;
    float chorusSend = 0.0f;
    float delaySend = 0.0f;
    
    // Вспомогательные методы
    inline void pushNote(uint8_t note) {
        if (stackSize < noteStack.size()) {
            noteStack[stackSize++] = note;
        }
    }
    
    inline void removeNote(uint8_t note) {
        for (uint8_t i = 0; i < stackSize; ++i) {
            if (noteStack[i] == note) {
                for (uint8_t j = i; j < stackSize - 1; ++j) {
                    noteStack[j] = noteStack[j + 1];
                }
                --stackSize;
                break;
            }
        }
    }
    
    inline uint8_t topNote() const {
        return stackSize > 0 ? noteStack[stackSize - 1] : 0xFF;
    }
    
    inline bool hasNotes() const {
        return stackSize > 0;
    }
    
    inline void clearNoteStack() {
        stackSize = 0;
    }
    
    inline uint16_t getBank() const {
        return (bankMSB << 7) | bankLSB;
    }
    
    inline uint16_t getWantBank() const {
        return (wantBankMSB << 7) | wantBankLSB;
    }
    
    inline void setBank(uint16_t bank) {
        bankMSB = (bank >> 7) & 0x7F;
        bankLSB = bank & 0x7F;
    }
    
    inline float getEffectiveVolume() const {
        return volume * expression;
    }
    
    inline void reset() {
        isDrum = false;
        volume = 1.0f;
        expression = 1.0f;
        pan = 0.5f;
        modWheel = 0.0f;
        pitchBend = 0.0f;
        pitchBendRange = 2.0f;
        pitchBendFactor = 1.0f;
        sustainPedal = 0;
        portamento = 0;
        portaTime = 0.2f;
        tuningSemitones = 0.0f;
        attackModifier = 1.0f;
        releaseModifier = 1.0f;
        monoMode = Poly;
        reverbSend = 0.0f;
        chorusSend = 0.0f;
        delaySend = 0.0f;
        clearNoteStack();
    }
    
    inline void activityIncrease(uint8_t vel) {
        activity += (float)vel / 127.0f * volume * expression;
        if (activity > 1.0f) activity = 1.0f;
    }
    
    inline void activityUpdate() {
        activity *= 0.95f;
    }
};

#endif // MIDI_CHANNEL_H