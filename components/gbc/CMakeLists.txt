idf_component_register(
    SRCS
        cpu.c
        gnuboy.c
        hw.c
        lcd.c
        sound.c
        gb_port.c
        gb_emulator.cpp
        gb_display_adapter.cpp
        gb_audio_task.cpp
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/Main"   # ← Абсолютный путь от корня проекта
    REQUIRES
        driver
        esp_timer
        freertos
        lvgl
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    -DRETRO_GO=1
    -DGB_USE_IRAM=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -ffast-math
    -fno-stack-protector
    -fno-common
    -fno-exceptions
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++17
)