#include "adsr.h"
#include <math.h>

ADSR::ADSR() : _sample_rate(44100), _segment(IDLE), _x(0), _target(0), _D0(0),
               _attackTarget(0), _attackD0(0), _decayD0(0), _releaseD0(0),
               _attackTime(0.01f), _holdTime(0), _decayTime(0.1f), _sustainLevel(0.7f), 
               _releaseTime(0.2f), _holdCounter(0), _holdSamples(0), _gate(false) {}

void ADSR::init(float sample_rate, int blockSize) {
    _sample_rate = sample_rate / blockSize;
    reset();
    
    _attackTime = 0.01f;
    _decayTime = 0.1f;
    _sustainLevel = 0.7f;
    _releaseTime = 0.2f;
    _holdTime = 0.0f;
    
    calcAttackD0();
    calcDecayD0();
    calcReleaseD0();
}

void ADSR::reset() {
    _segment = IDLE;
    _x = 0;
    _target = 0;
    _gate = false;
    _holdCounter = 0;
}

void ADSR::retrigger(EndType hardness) {
    _gate = true;
    _segment = ATTACK;
    _D0 = _attackD0;
    if (hardness == END_NOW) {
        _x = 0;
    }
}

void ADSR::release(EndType hardness) {
    _gate = false;
    _target = -0.1f;
    switch (hardness) {
        case END_NOW:
            _segment = IDLE;
            _x = 0;
            break;
        case END_FAST:
            _segment = RELEASE;
            _D0 = 0.5f;
            break;
        default:
            _segment = RELEASE;
            _D0 = _releaseD0;
            break;
    }
}

void ADSR::setAttackTime(float seconds, float shape) {
    _attackTime = seconds;
    (void)shape;
    calcAttackD0();
}

void ADSR::setHoldTime(float seconds) {
    _holdTime = seconds;
    if (seconds > 0) {
        _holdSamples = (uint32_t)(seconds * _sample_rate);
    } else {
        _holdSamples = 0;
    }
}

void ADSR::setDecayTime(float seconds) {
    _decayTime = seconds;
    calcDecayD0();
}

void ADSR::setSustainLevel(float level) {
    _sustainLevel = (level < 0) ? 0 : (level > 1) ? 1 : level;
}

void ADSR::setReleaseTime(float seconds) {
    _releaseTime = seconds;
    calcReleaseD0();
}

void ADSR::calcAttackD0() {
    if (_attackTime > 0) {
        float target = 9.0f * powf(_attackTime, 10.0f) + 0.3f * _attackTime + 1.01f;
        _attackTarget = target;
        float logTarget = logf(1.0f - (1.0f / target));
        _attackD0 = 1.0f - expf(logTarget / (_attackTime * _sample_rate));
    } else {
        _attackD0 = 1.0f;
        _attackTarget = 1.0f;
    }
}

void ADSR::calcDecayD0() {
    if (_decayTime > 0) {
        _decayD0 = 1.0f - expf(-1.0f / (0.2f * _decayTime * _sample_rate));
    } else {
        _decayD0 = 1.0f;
    }
}

void ADSR::calcReleaseD0() {
    if (_releaseTime > 0) {
        _releaseD0 = 1.0f - expf(-1.0f / (0.2f * _releaseTime * _sample_rate));
    } else {
        _releaseD0 = 1.0f;
    }
}

float ADSR::process() {
    float out = 0;
    
    switch (_segment) {
        case IDLE:
            out = 0;
            break;
            
        case ATTACK:
            _x += _D0 * (_attackTarget - _x);
            out = _x;
            if (out >= 1.0f) {
                _x = out = 1.0f;
                if (_holdSamples > 0) {
                    _segment = HOLD;
                    _holdCounter = _holdSamples;
                } else {
                    _segment = DECAY;
                    _target = _sustainLevel;
                    _D0 = _decayD0;
                }
            }
            break;
            
        case HOLD:
            out = _x;
            if (_holdCounter > 0) {
                _holdCounter--;
            } else {
                _segment = DECAY;
                _target = _sustainLevel;
                _D0 = _decayD0;
            }
            break;
            
        case DECAY:
        case RELEASE:
            _x += _D0 * (_target - _x);
            out = _x;
            if (out <= 0) {
                _segment = IDLE;
                _x = 0;
            }
            break;
            
        case SUSTAIN:
            out = _x;
            break;
    }
    
    return out;
}

bool ADSR::isActive() const {
    return _segment != IDLE;
}

bool ADSR::isIdle() const {
    return _segment == IDLE;
}

ADSR::Segment ADSR::getSegment() const {
    return _segment;
}

float ADSR::getValue() const {
    return _x;
}