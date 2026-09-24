#include "sf2_parser.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cmath>
#include <algorithm>

static const char* TAG = "SF2Parser";

#pragma pack(push, 1)
struct RIFFChunk {
    char id[4];
    uint32_t size;
};

struct RIFFHeader {
    char id[4];
    uint32_t size;
    char format[4];
};

// PDTA структуры
struct PHDR {
    char name[20];
    uint16_t preset;
    uint16_t bank;
    uint16_t bagIndex;
    uint32_t library;
    uint32_t genre;
    uint32_t morphology;
};

struct PBAG {
    uint16_t genIndex;
    uint16_t modIndex;
};

struct PGEN {
    uint16_t oper;
    int16_t amount;
};

struct PMOD {
    uint16_t srcOper;
    uint16_t destOper;
    int16_t amount;
    uint16_t amtSrcOper;
    uint16_t transOper;
};

struct INST {
    char name[20];
    uint16_t bagIndex;
};

struct IBAG {
    uint16_t genIndex;
    uint16_t modIndex;
};

struct IGEN {
    uint16_t oper;
    int16_t amount;
};

struct IMOD {
    uint16_t srcOper;
    uint16_t destOper;
    int16_t amount;
    uint16_t amtSrcOper;
    uint16_t transOper;
};

struct SHDR {
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
};
#pragma pack(pop)

// Проверка размеров структур
static_assert(sizeof(PHDR) == 38, "PHDR size mismatch");
static_assert(sizeof(PBAG) == 4, "PBAG size mismatch");
static_assert(sizeof(PGEN) == 4, "PGEN size mismatch");
static_assert(sizeof(PMOD) == 10, "PMOD size mismatch");
static_assert(sizeof(INST) == 22, "INST size mismatch");
static_assert(sizeof(IBAG) == 4, "IBAG size mismatch");
static_assert(sizeof(IGEN) == 4, "IGEN size mismatch");
static_assert(sizeof(IMOD) == 10, "IMOD size mismatch");
static_assert(sizeof(SHDR) == 46, "SHDR size mismatch");

static float timecentsToSec(int16_t tc) {
    if (tc <= -32768) return 0.0f;
    return powf(2.0f, tc / 1200.0f);
}

static float cBtoLin(int16_t cb) {
    return powf(10.0f, -cb / 200.0f);
}

static sf2::GeneratorOperator toGeneratorOperator(uint16_t oper) {
    if (oper <= static_cast<uint16_t>(sf2::GeneratorOperator::OverridingRootKey)) {
        return static_cast<sf2::GeneratorOperator>(oper);
    }
    return sf2::GeneratorOperator::StartAddrOffset;
}

static void decodeGeneratorAmount(sf2::Generator& gen, int16_t raw, uint16_t raw_u16 = 0) {
    auto op = toGeneratorOperator(gen.oper);

    switch (op) {
        case sf2::GeneratorOperator::Instrument:
        case sf2::GeneratorOperator::SampleID:
        case sf2::GeneratorOperator::SampleModes:
        case sf2::GeneratorOperator::ExclusiveClass:
        case sf2::GeneratorOperator::OverridingRootKey:
            gen.amount.uAmount = raw_u16 ? raw_u16 : static_cast<uint16_t>(raw);
            break;
        case sf2::GeneratorOperator::KeyRange:
        case sf2::GeneratorOperator::VelRange:
            gen.amount.range.lo = raw & 0xFF;
            gen.amount.range.hi = (raw >> 8) & 0xFF;
            break;
        default:
            gen.amount.sAmount = raw;
            break;
    }
}

SF2Parser::SF2Parser(const char* path) : _path(path), _parsed(false) {}

SF2Parser::~SF2Parser() {
    clear();
}

void SF2Parser::clear() {
    for (auto& s : _samples) {
        if (s.data) {
            heap_caps_free(s.data);
            s.data = nullptr;
        }
    }
    _samples.clear();
    _presets.clear();
    _instruments.clear();
    _loaded_presets.clear();
    _parsed = false;
}

bool SF2Parser::parse() {
    if (_parsed) return true;
    
    FILE* f = fopen(_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open SF2 file: %s", _path.c_str());
        return false;
    }
    
    if (!parse_structures(f)) {
        fclose(f);
        return false;
    }
    
    // Загружаем все сэмплы
    std::set<uint32_t> all_samples;
    for (size_t i = 0; i < _samples.size(); i++) {
        all_samples.insert(i);
    }
    load_samples_lazy(f, all_samples);
    
    fclose(f);
    _parsed = true;
    return true;
}

bool SF2Parser::parse_lazy(const uint16_t* banks, const uint16_t* programs, int count) {
    if (_parsed) return true;
    
    FILE* f = fopen(_path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open SF2 file: %s", _path.c_str());
        return false;
    }
    
    /* 1. Парсим только структуры (без загрузки сэмплов) */
    if (!parse_structures(f)) {
        fclose(f);
        return false;
    }
    
    /* 2. Отмечаем нужные пресеты */
    _loaded_presets.clear();
    for (int i = 0; i < count; i++) {
        uint32_t key = (banks[i] << 16) | programs[i];
        _loaded_presets.insert(key);
        ESP_LOGI(TAG, "Marked preset: bank=%d, program=%d", banks[i], programs[i]);
    }
    
    /* 3. Собираем список нужных сэмплов - ПРОХОДИМ ПО ВСЕМ ЗОНАМ И ГЛОБАЛЬНЫМ */
    std::set<uint32_t> needed_samples;
    
    /* Вспомогательная лямбда для добавления всех сэмплов из инструмента */
    auto add_instrument_samples = [&](const SF2Instrument& inst, const std::string& source) {
        /* Из зон инструмента */
        for (const auto& izone : inst.zones) {
            for (const auto& ig : izone.generators) {
                if (toGeneratorOperator(ig.oper) == sf2::GeneratorOperator::SampleID) {
                    needed_samples.insert(ig.amount.uAmount);
                    ESP_LOGD(TAG, "    Added sample %d from %s zone", ig.amount.uAmount, source.c_str());
                }
            }
        }
        /* Из глобальных генераторов инструмента */
        for (const auto& ig : inst.globalGenerators) {
            if (toGeneratorOperator(ig.oper) == sf2::GeneratorOperator::SampleID) {
                needed_samples.insert(ig.amount.uAmount);
                ESP_LOGD(TAG, "    Added sample %d from %s global", ig.amount.uAmount, source.c_str());
            }
        }
    };
    
    for (const auto& preset : _presets) {
        uint32_t key = (preset.bank << 16) | preset.program;
        if (_loaded_presets.find(key) != _loaded_presets.end()) {
            ESP_LOGI(TAG, "Processing preset: %s (bank=%d, prog=%d)", 
                     preset.name.c_str(), preset.bank, preset.program);
            
            /* 3a. Проверяем зоны пресета */
            for (const auto& pzone : preset.zones) {
                for (const auto& g : pzone.generators) {
                    if (toGeneratorOperator(g.oper) == sf2::GeneratorOperator::Instrument) {
                        int instIndex = g.amount.uAmount;
                        if (instIndex >= 0 && instIndex < (int)_instruments.size()) {
                            const auto& inst = _instruments[instIndex];
                            ESP_LOGI(TAG, "  Instrument from zone: %s", inst.name.c_str());
                            add_instrument_samples(inst, "zone");
                        }
                    }
                }
            }
            
            /* 3b. Проверяем глобальные генераторы пресета */
            for (const auto& g : preset.globalGenerators) {
                if (toGeneratorOperator(g.oper) == sf2::GeneratorOperator::Instrument) {
                    int instIndex = g.amount.uAmount;
                    if (instIndex >= 0 && instIndex < (int)_instruments.size()) {
                        const auto& inst = _instruments[instIndex];
                        ESP_LOGI(TAG, "  Instrument from global: %s", inst.name.c_str());
                        add_instrument_samples(inst, "global");
                    }
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "Need %zu samples out of %zu total", needed_samples.size(), _samples.size());
    
    /* 4. Загружаем только нужные сэмплы */
    if (!needed_samples.empty()) {
        bool loaded = load_samples_lazy(f, needed_samples);
        if (!loaded) {
            ESP_LOGE(TAG, "Failed to load samples lazily");
            fclose(f);
            return false;
        }
    } else {
        ESP_LOGW(TAG, "No samples needed - loading all samples");
        std::set<uint32_t> all_samples;
        for (size_t i = 0; i < _samples.size(); i++) {
            all_samples.insert(i);
        }
        load_samples_lazy(f, all_samples);
    }
    
    fclose(f);
    _parsed = true;
    return true;
}


bool SF2Parser::parse_structures(FILE* f) {
    // Читаем RIFF заголовок
    RIFFHeader riff;
    if (fread(&riff, sizeof(riff), 1, f) != 1) {
        return false;
    }
    
    if (memcmp(riff.id, "RIFF", 4) != 0 || memcmp(riff.format, "sfbk", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid SF2 file");
        return false;
    }
    
    // Ищем pdta и sdta
    uint32_t pdtaOffset = 0, pdtaSize = 0;
    uint32_t sdtaOffset = 0, sdtaSize = 0;
    uint32_t smplOffset = 0, smplSize = 0;  // ★ НОВОЕ
    
    while (!feof(f)) {
        RIFFChunk chunk;
        if (fread(&chunk, sizeof(chunk), 1, f) != 1) break;
        
        if (memcmp(chunk.id, "LIST", 4) == 0) {
            char listType[5] = {0};
            if (fread(listType, 4, 1, f) != 1) break;
            
            if (memcmp(listType, "pdta", 4) == 0) {
                pdtaOffset = ftell(f);
                pdtaSize = chunk.size - 4;
                fseek(f, pdtaSize, SEEK_CUR);
            } else if (memcmp(listType, "sdta", 4) == 0) {
                sdtaOffset = ftell(f);
                sdtaSize = chunk.size - 4;
                ESP_LOGI(TAG, "sdta found at offset %u, size %u", sdtaOffset, sdtaSize);
                
                /* ★ Сканируем внутри sdta для поиска smpl ★ */
                long sdtaEnd = ftell(f) + sdtaSize;
                while (ftell(f) < sdtaEnd) {
                    char subid[5] = {0};
                    uint32_t subsize;
                    if (fread(subid, 4, 1, f) != 1) break;
                    if (fread(&subsize, 4, 1, f) != 1) break;
                    
                    if (memcmp(subid, "smpl", 4) == 0) {
                        smplOffset = ftell(f);
                        smplSize = subsize;
                        ESP_LOGI(TAG, "smpl found at offset %u, size %u", smplOffset, smplSize);
                        fseek(f, subsize, SEEK_CUR);
                    } else {
                        fseek(f, subsize, SEEK_CUR);
                    }
                }
            } else {
                fseek(f, chunk.size - 4, SEEK_CUR);
            }
        } else if (memcmp(chunk.id, "smpl", 4) == 0) {
            /* ★ smpl на верхнем уровне ★ */
            smplOffset = ftell(f);
            smplSize = chunk.size;
            ESP_LOGI(TAG, "smpl found at top level, offset %u, size %u", smplOffset, smplSize);
            fseek(f, chunk.size, SEEK_CUR);
        } else {
            fseek(f, chunk.size, SEEK_CUR);
        }
    }
    
    if (pdtaOffset == 0) {
        ESP_LOGE(TAG, "PDTA not found");
        return false;
    }
    
    // ========== ЧИТАЕМ PDTA ==========
    fseek(f, pdtaOffset, SEEK_SET);
    long pdtaEnd = pdtaOffset + pdtaSize;
    
    std::vector<PHDR> phdrs;
    std::vector<PBAG> pbags;
    std::vector<PGEN> pgens;
    std::vector<PMOD> pmods;
    std::vector<INST> insts;
    std::vector<IBAG> ibags;
    std::vector<IGEN> igens;
    std::vector<IMOD> imods;
    std::vector<SHDR> shdrs;
    
    while (ftell(f) + 8 <= pdtaEnd) {
        char id[5] = {0};
        uint32_t size;
        
        if (fread(id, 4, 1, f) != 1) break;
        if (fread(&size, 4, 1, f) != 1) break;
        
        if (memcmp(id, "phdr", 4) == 0) {
            phdrs.resize(size / sizeof(PHDR));
            fread(phdrs.data(), size, 1, f);
            ESP_LOGI(TAG, "phdr: %zu entries", phdrs.size());
        }
        else if (memcmp(id, "pbag", 4) == 0) {
            pbags.resize(size / sizeof(PBAG));
            fread(pbags.data(), size, 1, f);
            ESP_LOGI(TAG, "pbag: %zu entries", pbags.size());
        }
        else if (memcmp(id, "pmod", 4) == 0) {
            pmods.resize(size / sizeof(PMOD));
            fread(pmods.data(), size, 1, f);
            ESP_LOGD(TAG, "pmod: %zu entries", pmods.size());
        }
        else if (memcmp(id, "pgen", 4) == 0) {
            pgens.resize(size / sizeof(PGEN));
            fread(pgens.data(), size, 1, f);
            ESP_LOGI(TAG, "pgen: %zu entries", pgens.size());
        }
        else if (memcmp(id, "inst", 4) == 0) {
            insts.resize(size / sizeof(INST));
            fread(insts.data(), size, 1, f);
            ESP_LOGI(TAG, "inst: %zu entries", insts.size());
        }
        else if (memcmp(id, "ibag", 4) == 0) {
            ibags.resize(size / sizeof(IBAG));
            fread(ibags.data(), size, 1, f);
            ESP_LOGI(TAG, "ibag: %zu entries", ibags.size());
        }
        else if (memcmp(id, "imod", 4) == 0) {
            imods.resize(size / sizeof(IMOD));
            fread(imods.data(), size, 1, f);
            ESP_LOGD(TAG, "imod: %zu entries", imods.size());
        }
        else if (memcmp(id, "igen", 4) == 0) {
            igens.resize(size / sizeof(IGEN));
            fread(igens.data(), size, 1, f);
            ESP_LOGI(TAG, "igen: %zu entries", igens.size());
        }
        else if (memcmp(id, "shdr", 4) == 0) {
            shdrs.resize(size / sizeof(SHDR));
            fread(shdrs.data(), size, 1, f);
            ESP_LOGI(TAG, "shdr: %zu entries", shdrs.size());
        }
        else {
            fseek(f, size, SEEK_CUR);
        }
        
        if (size % 2) fseek(f, 1, SEEK_CUR);
    }
    
    // Добавляем терминаторы
    if (!phdrs.empty()) {
        PHDR termPhdr = {};
        termPhdr.bagIndex = pbags.size();
        phdrs.push_back(termPhdr);
    }
    
    if (!pbags.empty()) {
        PBAG termPbag = {};
        termPbag.genIndex = pgens.size();
        termPbag.modIndex = pmods.size();
        pbags.push_back(termPbag);
    }
    
    if (!insts.empty()) {
        INST termInst = {};
        termInst.bagIndex = ibags.size();
        insts.push_back(termInst);
    }
    
    if (!ibags.empty()) {
        IBAG termIbag = {};
        termIbag.genIndex = igens.size();
        termIbag.modIndex = imods.size();
        ibags.push_back(termIbag);
    }
    
    // ========== СТРОИМ СЭМПЛЫ ==========
    _samples.resize(shdrs.size());
    for (size_t i = 0; i < shdrs.size(); i++) {
        memcpy(_samples[i].name, shdrs[i].name, 20);
        _samples[i].name[19] = '\0';
        _samples[i].start = shdrs[i].start;
        _samples[i].end = shdrs[i].end;
        _samples[i].startLoop = shdrs[i].startLoop;
        _samples[i].endLoop = shdrs[i].endLoop;
        _samples[i].sampleRate = shdrs[i].sampleRate;
        _samples[i].originalPitch = shdrs[i].originalPitch;
        _samples[i].pitchCorrection = shdrs[i].pitchCorrection;
        _samples[i].sampleLink = shdrs[i].sampleLink;
        _samples[i].sampleType = shdrs[i].sampleType;
        _samples[i].data = nullptr;
        _samples[i].dataSize = 0;
    }
    
    // ========== СТРОИМ ПРЕСЕТЫ ==========
    for (size_t i = 0; i + 1 < phdrs.size(); i++) {
        SF2Preset preset;
        preset.name = std::string(phdrs[i].name, strnlen(phdrs[i].name, 20));
        preset.bank = phdrs[i].bank;
        preset.program = phdrs[i].preset;
        
        bool firstZone = true;
        
        for (uint16_t b = phdrs[i].bagIndex; b < phdrs[i + 1].bagIndex; b++) {
            SF2Zone zone;
            bool hasInstrument = false;
            
            for (uint16_t g = pbags[b].genIndex; g < pbags[b + 1].genIndex; g++) {
                sf2::Generator gen;
                gen.oper = pgens[g].oper;
                decodeGeneratorAmount(gen, pgens[g].amount);
                
                if (toGeneratorOperator(gen.oper) == sf2::GeneratorOperator::Instrument) {
                    hasInstrument = true;
                }
                
                zone.generators.push_back(gen);
            }
            
            // Глобальная зона (первая зона пресета)
            if (!hasInstrument && firstZone) {
                preset.globalGenerators = zone.generators;
                firstZone = false;
            } else if (hasInstrument) {
                preset.zones.push_back(zone);
            }
        }
        
        _presets.push_back(preset);
    }
    
    // ========== СТРОИМ ИНСТРУМЕНТЫ ==========
    for (size_t i = 0; i + 1 < insts.size(); i++) {
        SF2Instrument inst;
        inst.name = std::string(insts[i].name, strnlen(insts[i].name, 20));
        
        bool firstZone = true;
        
        for (uint16_t b = insts[i].bagIndex; b < insts[i + 1].bagIndex; b++) {
            SF2Zone zone;
            bool hasSampleID = false;
            
            for (uint16_t g = ibags[b].genIndex; g < ibags[b + 1].genIndex; g++) {
                sf2::Generator gen;
                gen.oper = igens[g].oper;
                decodeGeneratorAmount(gen, igens[g].amount);
                
                if (toGeneratorOperator(gen.oper) == sf2::GeneratorOperator::SampleID) {
                    hasSampleID = true;
                }
                
                zone.generators.push_back(gen);
            }
            
            if (!hasSampleID && firstZone) {
                inst.globalGenerators = zone.generators;
                firstZone = false;
            } else if (hasSampleID) {
                inst.zones.push_back(zone);
            }
        }
        
        _instruments.push_back(inst);
    }
    
    ESP_LOGI(TAG, "Parsed %zu presets, %zu instruments, %zu samples", 
             _presets.size(), _instruments.size(), _samples.size());
    
    _smplOffset = smplOffset;
    _smplSize = smplSize;

    return true;
}

bool SF2Parser::load_samples_lazy(FILE* f, const std::set<uint32_t>& needed_samples) {
    if (_smplOffset == 0 || _smplSize == 0) {
        ESP_LOGE(TAG, "smpl chunk not found during parsing!");
        return false;
    }
    
    ESP_LOGI(TAG, "Loading samples from smpl chunk at offset %u, size %u", _smplOffset, _smplSize);
    
    fseek(f, _smplOffset, SEEK_SET);
    
    int loaded_count = 0;
    for (size_t i = 0; i < _samples.size(); i++) {
        if (needed_samples.find(i) == needed_samples.end()) {
            continue;
        }
        
        auto& s = _samples[i];
        uint32_t length = s.end - s.start;
        
        if (length == 0) {
            ESP_LOGW(TAG, "Sample %zu '%s' has zero length", i, s.name);
            continue;
        }
        
        /* Проверяем, что сэмпл не выходит за пределы smpl чанка */
        if (s.start * 2 + length * 2 > _smplSize) {
            ESP_LOGW(TAG, "Sample %zu '%s' extends beyond smpl chunk (%u > %u)", 
                     i, s.name, s.start * 2 + length * 2, _smplSize);
            continue;
        }
        
        s.data = (int16_t*)heap_caps_malloc(length * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.data) {
            ESP_LOGE(TAG, "Failed to allocate memory for sample %zu (%s)", i, s.name);
            continue;
        }
        
        fseek(f, _smplOffset + s.start * 2, SEEK_SET);
        size_t read_count = fread(s.data, 2, length, f);
        s.dataSize = read_count * 2;
        ESP_LOGI(TAG, "Loaded sample %zu: %s (%u bytes)", i, s.name, s.dataSize);
        loaded_count++;
    }
    
    ESP_LOGI(TAG, "Loaded %d samples out of %zu needed", loaded_count, needed_samples.size());
    return loaded_count > 0;
}


void SF2Parser::applyGenerators(const std::vector<sf2::Generator>& gens, sf2::Zone& zone) {
    for (const auto& g : gens) {
        auto op = toGeneratorOperator(g.oper);
        int16_t val = g.amount.sAmount;
        
        switch (op) {
            case sf2::GeneratorOperator::SampleID:
                // sample будет установлен в getZonesForNote
                break;
            case sf2::GeneratorOperator::KeyRange:
                zone.keyLo = g.amount.range.lo;
                zone.keyHi = g.amount.range.hi;
                break;
            case sf2::GeneratorOperator::VelRange:
                zone.velLo = g.amount.range.lo;
                zone.velHi = g.amount.range.hi;
                break;
            case sf2::GeneratorOperator::OverridingRootKey:
                zone.rootKey = val;
                break;
            case sf2::GeneratorOperator::SampleModes:
                zone.sampleModes = val;
                break;
            case sf2::GeneratorOperator::StartLoopAddrOffset:
                zone.loopStartOffset = val;
                break;
            case sf2::GeneratorOperator::EndLoopAddrOffset:
                zone.loopEndOffset = val;
                break;
            case sf2::GeneratorOperator::StartLoopAddrCoarseOffset:
                zone.loopStartCoarseOffset = val;
                break;
            case sf2::GeneratorOperator::EndLoopAddrCoarseOffset:
                zone.loopEndCoarseOffset = val;
                break;
            case sf2::GeneratorOperator::ExclusiveClass:
                zone.exclusiveClass = val;
                break;
            case sf2::GeneratorOperator::FineTune:
                zone.fineTune = val / 100.0f;
                break;
            case sf2::GeneratorOperator::CoarseTune:
                zone.coarseTune = val;
                break;
            case sf2::GeneratorOperator::AttackVolEnv:
                zone.attackTime = timecentsToSec(val);
                break;
            case sf2::GeneratorOperator::HoldVolEnv:
                zone.holdTime = timecentsToSec(val);
                break;
            case sf2::GeneratorOperator::DecayVolEnv:
                zone.decayTime = timecentsToSec(val);
                break;
            case sf2::GeneratorOperator::SustainVolEnv:
                zone.sustainLevel = cBtoLin(val);
                break;
            case sf2::GeneratorOperator::ReleaseVolEnv:
                zone.releaseTime = timecentsToSec(val);
                break;
            case sf2::GeneratorOperator::Pan:
                zone.pan = val / 1000.0f;
                break;
            case sf2::GeneratorOperator::InitialAttenuation:
                zone.attenuation = powf(10.0f, -val / 200.0f);
                break;
            case sf2::GeneratorOperator::ReverbEffectsSend:
                zone.reverbSend = val / 1000.0f;
                break;
            case sf2::GeneratorOperator::ChorusEffectsSend:
                zone.chorusSend = val / 1000.0f;
                break;
            default:
                break;
        }
    }
}

sf2::SampleHeader* SF2Parser::getSample(uint32_t id) {
    if (id < _samples.size()) {
        return &_samples[id];
    }
    return nullptr;
}

std::vector<sf2::Zone> SF2Parser::getZonesForNote(uint8_t channel, uint8_t note, uint8_t velocity,
                                                   uint16_t bank, uint16_t program) {
    std::vector<sf2::Zone> resultZones;
    
    // Для ударных (канал 9) используем банк 128
    if (channel == 9) {
        bank = 128;
        program = 0;
    }
    
    for (const auto& preset : _presets) {
        if (preset.bank != bank || preset.program != program) continue;
        
        // Безопасный вывод названия пресета
        if (!preset.name.empty()) {
            ESP_LOGD(TAG, "Found preset: %s", preset.name.c_str());
        }
        
        for (const auto& pzone : preset.zones) {
            int instIndex = -1;
            for (const auto& g : pzone.generators) {
                if (toGeneratorOperator(g.oper) == sf2::GeneratorOperator::Instrument) {
                    instIndex = g.amount.uAmount;
                    break;
                }
            }
            
            if (instIndex < 0 || instIndex >= (int)_instruments.size()) {
                ESP_LOGD(TAG, "Invalid instrument index %d", instIndex);
                continue;
            }
            
            const auto& inst = _instruments[instIndex];
            
            for (const auto& izone : inst.zones) {
                int sampleIndex = -1;
                uint8_t keyLo = 0, keyHi = 127;
                uint8_t velLo = 0, velHi = 127;
                
                for (const auto& g : izone.generators) {
                    auto oper = toGeneratorOperator(g.oper);
                    if (oper == sf2::GeneratorOperator::SampleID) {
                        sampleIndex = g.amount.uAmount;
                    } else if (oper == sf2::GeneratorOperator::KeyRange) {
                        keyLo = g.amount.range.lo;
                        keyHi = g.amount.range.hi;
                    } else if (oper == sf2::GeneratorOperator::VelRange) {
                        velLo = g.amount.range.lo;
                        velHi = g.amount.range.hi;
                    }
                }
                
                // Проверяем, что сэмпл существует и имеет данные
                if (sampleIndex >= 0 && sampleIndex < (int)_samples.size()) {
                    const auto& sample = _samples[sampleIndex];
                    
                    // Проверка на NULL data
if (sample.data == nullptr) {
    ESP_LOGW(TAG, "Sample %d has no data, loading on demand", sampleIndex);
    /* Загружаем этот сэмпл сейчас */
    FILE* f = fopen(_path.c_str(), "rb");
    if (f) {
        std::set<uint32_t> single_sample;
        single_sample.insert(sampleIndex);
        load_samples_lazy(f, single_sample);
        fclose(f);
        if (_samples[sampleIndex].data != nullptr) {
            ESP_LOGI(TAG, "Sample %d loaded on demand successfully", sampleIndex);
            /* Создаём зону */
            sf2::Zone z;
            z.sample = &_samples[sampleIndex];
            // ... остальные параметры ...
            resultZones.push_back(z);
            continue;
        }
    }
    continue;
}
                    
                    // Проверка попадания в диапазоны
                    if (note >= keyLo && note <= keyHi &&
                        velocity >= velLo && velocity <= velHi) {
                        
                        sf2::Zone z;
                        z.sample = &_samples[sampleIndex];
                        z.keyLo = keyLo;
                        z.keyHi = keyHi;
                        z.velLo = velLo;
                        z.velHi = velHi;
                        z.rootKey = _samples[sampleIndex].originalPitch;
                        
                        // Применяем генераторы в правильном порядке
                        applyGenerators(preset.globalGenerators, z);
                        applyGenerators(pzone.generators, z);
                        applyGenerators(inst.globalGenerators, z);
                        applyGenerators(izone.generators, z);
                        
                        const char* sample_name = "Unknown";
                        if (z.sample && z.sample->name[0] != '\0') {
                            sample_name = z.sample->name;
                        }
                        ESP_LOGD(TAG, "Mapped: note=%d vel=%d -> sample=%s", 
                                 note, velocity, sample_name);
                        resultZones.push_back(z);
                    }
                }
            }
        }
        break;
    }
    
    return resultZones;
}

bool SF2Parser::hasPreset(uint16_t bank, uint16_t program) const {
    for (const auto& p : _presets) {
        if (p.bank == bank && p.program == program) return true;
    }
    return false;
}

void SF2Parser::dumpInfo() {
    ESP_LOGI(TAG, "=== SF2 Info ===");
    ESP_LOGI(TAG, "Samples: %zu", _samples.size());
    ESP_LOGI(TAG, "Presets: %zu", _presets.size());
    ESP_LOGI(TAG, "Instruments: %zu", _instruments.size());
    ESP_LOGI(TAG, "Loaded presets: %zu", _loaded_presets.size());
    
    for (const auto& p : _presets) {
        uint32_t key = (p.bank << 16) | p.program;
        bool loaded = _loaded_presets.find(key) != _loaded_presets.end();
        ESP_LOGD(TAG, "Preset: %s (bank=%d, prog=%d, zones=%zu) %s", 
                 p.name.c_str(), p.bank, p.program, p.zones.size(),
                 loaded ? "[LOADED]" : "");
    }
}