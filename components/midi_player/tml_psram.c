/* TinyMidiLoader with PSRAM allocation */
#include "esp_heap_caps.h"

#define TML_IMPLEMENTATION
#define TML_MALLOC(size)   heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TML_FREE(ptr)      heap_caps_free(ptr)
#define TML_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#include "tml.h"