#ifndef SF2_PARSER_H
#define SF2_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <set>
#include "sf2_types.h"

struct SF2Zone {
    std::vector<sf2::Generator> generators;
};

struct SF2Preset {
    std::string name;
    uint16_t bank;
    uint16_t program;
    std::vector<SF2Zone> zones;
    std::vector<sf2::Generator> globalGenerators;
};

struct SF2Instrument {
    std::string name;
    std::vector<SF2Zone> zones;
    std::vector<sf2::Generator> globalGenerators;
};

class SF2Parser {
public:
    explicit SF2Parser(const char* path);
    ~SF2Parser();
    
    /* Полный парсинг (все сэмплы) */
    bool parse();
    
    /* Ленивый парсинг (только нужные пресеты) */
    bool parse_lazy(const uint16_t* banks, const uint16_t* programs, int count);
    
    /* Очистка */
    void clear();
    
    /* Получение зон для ноты */
    std::vector<sf2::Zone> getZonesForNote(uint8_t channel, uint8_t note, uint8_t velocity,
                                            uint16_t bank, uint16_t program);
    
    /* Проверка наличия пресета */
    bool hasPreset(uint16_t bank, uint16_t program) const;
    
    /* Информация */
    void dumpInfo();
    
    const std::vector<SF2Preset>& getPresets() const { return _presets; }
    const std::vector<SF2Instrument>& getInstruments() const { return _instruments; }
    
private:
    
    uint32_t _smplOffset = 0;
    uint32_t _smplSize = 0; 

    
/* Парсинг структур (без загрузки сэмплов) */
    bool parse_structures(FILE* f);
    
    /* Загрузка только нужных сэмплов */
    bool load_samples_lazy(FILE* f, const std::set<uint32_t>& needed_samples);
    
    /* Применение генераторов */
    void applyGenerators(const std::vector<sf2::Generator>& gens, sf2::Zone& zone);
    
    /* Получение сэмпла по ID */
    sf2::SampleHeader* getSample(uint32_t id);
    
    std::string _path;
    std::vector<sf2::SampleHeader> _samples;
    std::vector<SF2Preset> _presets;
    std::vector<SF2Instrument> _instruments;
    std::set<uint32_t> _loaded_presets;  /* Загруженные пресеты */
    bool _parsed;
};

#endif // SF2_PARSER_H