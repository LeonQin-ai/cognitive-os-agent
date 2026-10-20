/* Local portability shim: strnlen is POSIX, not ISO C11. */
#pragma once
#include <stddef.h>
static inline size_t m3_libc_strnlen(const char *text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != '\0') length++;
    return length;
}
