# CMakeLists.txt for GME component for ESP-IDF

idf_component_register(
    SRCS
        # Core files
        Blip_Buffer.cpp
        Classic_Emu.cpp
        Data_Reader.cpp
        Dual_Resampler.cpp
        Effects_Buffer.cpp
        Fir_Resampler.cpp
        gme.cpp
        Gme_File.cpp
        M3u_Playlist.cpp
        Multi_Buffer.cpp
        Music_Emu.cpp
        # YM2612 emulator (нужен для VGM/GYM)
        Ym2612_GENS.cpp
        
        # Ay_Apu
        Ay_Apu.cpp
        Ay_Cpu.cpp
        Ay_Emu.cpp
        
        # GBS
        Gb_Apu.cpp
        Gb_Cpu.cpp
        Gb_Oscs.cpp
        Gbs_Emu.cpp
        
        # GYM
        Gym_Emu.cpp
        
        # HES
        Hes_Apu.cpp
        Hes_Cpu.cpp
        Hes_Emu.cpp
        
        # KSS
        Kss_Cpu.cpp
        Kss_Emu.cpp
        Kss_Scc_Apu.cpp
        
        # NSF/NSFE
        Nes_Apu.cpp
        Nes_Cpu.cpp
        Nes_Fme7_Apu.cpp
        Nes_Namco_Apu.cpp
        Nes_Oscs.cpp
        Nes_Vrc6_Apu.cpp
        Nsf_Emu.cpp
        Nsfe_Emu.cpp
        
        # SAP
        Sap_Apu.cpp
        Sap_Cpu.cpp
        Sap_Emu.cpp
        
        # SPC
        Snes_Spc.cpp
        Spc_Cpu.cpp
        Spc_Dsp.cpp
        Spc_Emu.cpp
        Spc_Filter.cpp
        
        # Sms_Apu
        Sms_Apu.cpp
        
        # VGM
        Vgm_Emu.cpp
        Vgm_Emu_Impl.cpp
        Ym2413_Emu.cpp
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
    zlib
)

# Добавляем необходимые определения
target_compile_definitions(${COMPONENT_TARGET} PRIVATE 
    -DBLARGG_BUILD_DLL
    -DVGM_YM2612_GENS
    -DHAVE_ZLIB_H      # ← КЛЮЧЕВОЙ ФЛАГ для поддержки VGZ!
    -DUSE_GME_VGM
    -DUSE_GME_GYM
    -DUSE_GME_GBS
    -DUSE_GME_NSF
    -DUSE_GME_SPC
    -DUSE_GME_AY
    -DUSE_GME_HES
    -DUSE_GME_KSS
    -DUSE_GME_SAP
)