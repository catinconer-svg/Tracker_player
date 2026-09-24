#ifndef ADSR_H
#define ADSR_H

#include <stdint.h>

class ADSR {
public:
    enum Segment { IDLE, ATTACK, HOLD, DECAY, SUSTAIN, RELEASE };
    enum EndType { END_REGULAR, END_FAST, END_NOW };
    
    ADSR();
    
    void init(float sample_rate, int blockSize = 1);
    void reset();
    void retrigger(EndType hardness);
    void release(EndType hardness);
    
    void setAttackTime(float seconds, float shape = 0.0f);
    void setHoldTime(float seconds);
    void setDecayTime(float seconds);
    void setSustainLevel(float level);
    void setReleaseTime(float seconds);
    
    float process();
    bool isActive() const;
    bool isIdle() const;
    Segment getSegment() const;
    float getValue() const;
    
private:
    void calcAttackD0();
    void calcDecayD0();
    void calcReleaseD0();
    
    float _sample_rate;
    Segment _segment;
    
    float _x;           // текущее значение
    float _target;      // целевое значение
    float _D0;          // коэффициент
    float _attackTarget;
    float _attackD0;
    float _decayD0;
    float _releaseD0;
    
    float _attackTime;
    float _holdTime;
    float _decayTime;
    float _sustainLevel;
    float _releaseTime;
    
    uint32_t _holdCounter;
    uint32_t _holdSamples;
    
    bool _gate;
};

#endif // ADSR_H