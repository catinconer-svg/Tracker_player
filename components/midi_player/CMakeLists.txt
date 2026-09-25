idf_component_register(
    SRCS
        midi_player.cpp
        tml_psram.c
        sf2_parser.cpp
        synth.cpp
        voice.cpp
        adsr.cpp
        usb_midi.cpp
    INCLUDE_DIRS
        "."
        "../../managed_components/espressif__esp_tinyusb/include"
        "../../managed_components/espressif__tinyusb/src"
    REQUIRES
        tinyusb
)

target_compile_definitions(${COMPONENT_TARGET} PRIVATE 
    -DTML_IMPLEMENTATION
    -DBOARD_HAS_PSRAM
    -DCONFIG_TINYUSB_MIDI_COUNT=1
    -DCONFIG_TINYUSB_MIDI_ENABLED=1
)

target_compile_options(${COMPONENT_TARGET} PRIVATE 
    -Wno-format-truncation
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-unknown-pragmas
    -Wno-unused-but-set-variable
    -ffast-math
    -funroll-loops
)