set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LOADERS_DIR "${SRC_DIR}/loaders")
set(DEPACKERS_DIR "${SRC_DIR}/depackers")

# Собираем основные исходные файлы
file(GLOB SRC_FILES "${SRC_DIR}/*.c")
file(GLOB LOADER_FILES "${LOADERS_DIR}/*.c")

# !!! ВАЖНО: НЕ включаем файлы распаковщиков, так как определяем LIBXMP_NO_DEPACKERS
# file(GLOB DEPACKER_FILES "${DEPACKERS_DIR}/*.c")  <-- ЗАКОММЕНТИРОВАНО!

# Убираем win32.c, чтобы не было конфликтов
list(REMOVE_ITEM SRC_FILES "${SRC_DIR}/win32.c")

idf_component_register(
    SRCS ${SRC_FILES} ${LOADER_FILES}
    # DEPACKER_FILES не включаем!
    INCLUDE_DIRS "include" "src"
    REQUIRES fatfs esp_timer driver
)

# Определяем макросы для отключения распаковщиков
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    XMP_CORE_PLAYER
    XMP_NO_MMAP
    XMP_NO_DEPACKERS      # Отключаем распаковщики
    LIBXMP_NO_DEPACKERS   # Дублируем для надёжности
    LIBXMP_NO_PROWIZARD   # Отключаем ProWizard
    _CRT_SECURE_NO_WARNINGS
    ICE_INLINE=
    ICE_RESTRICT=
)