#ifndef MIDI_SYNTH_H
#define MIDI_SYNTH_H

#include <vector>
#include "sf2_types.h"
#include "sf2_parser.h"
#include "voice.h"
#include "channel.h"
#include "config.h"

class Synth {
public:
    Synth(SF2Parser& parser);
    ~Synth();
    
    bool init();
    void reset();
    void GMReset();
    
    // MIDI события
    void noteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOff(uint8_t channel, uint8_t note);
    void controlChange(uint8_t channel, uint8_t control, uint8_t value);
    void programChange(uint8_t channel, uint8_t program);
    void pitchBend(uint8_t channel, int value);
    
    // Аудио рендеринг
    void renderBlock(float* outL, float* outR, int samples);
    
    // Состояние
    ChannelState& getChannel(uint8_t ch) { return _channels[ch]; }
    
private:
    Voice* allocateVoice(uint8_t channel, uint8_t note, float score, uint32_t exclusiveClass);
    Voice* findWeakestVoice(uint8_t channel, uint8_t note, float score, uint32_t exclusiveClass);
    Voice* findWorstVoice();
    void applyBankProgram(uint8_t channel);
    std::vector<sf2::Zone> getZonesForNote(uint8_t channel, uint8_t note, uint8_t velocity,
                                        uint16_t bank, uint16_t program);
    SF2Parser& _parser;
    ChannelState _channels[16];
    Voice _voices[MAX_VOICES];
    
    float _volumeScaler;
    bool _sf2Loaded;
};

#endif // MIDI_SYNTH_H