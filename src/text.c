#include "text.h"
#include "memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

char *weivel_text_format(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list measuring;
    va_copy(measuring, arguments);
    int length = vsnprintf(NULL, 0, format, measuring);
    va_end(measuring);
    char *text = length < 0 ? NULL : weivel_memory_allocate((size_t)length + 1);
    if (text != NULL) {
        vsnprintf(text, (size_t)length + 1, format, arguments);
    }
    va_end(arguments);
    return text;
}
