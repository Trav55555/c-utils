#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cu.h"
#include "curun.h"

int fuzz_curun_main(int argc, char *argv[], char *envp[]);
#define curun_main fuzz_curun_main
#include "../src/curun.c"
#undef curun_main

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char text[129];
    char bound[sizeof(unsigned long) * 3 + 1];
    const char *significant;
    size_t i;
    size_t length;
    unsigned long max;
    unsigned long value;
    int valid;
    int actual;

    if (size == 0)
        return 0;
    max = data[0] % 2 == 0 ? ULONG_MAX : CURUN_MAX_CAPTURE;
    size--;
    if (size > 128)
        size = 128;
    for (i = 0; i < size; i++)
        text[i] = (char)data[i + 1];
    text[size] = '\0';
    valid = text[0] != '\0';
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9')
            valid = 0;
    }
    significant = text;
    while (*significant == '0')
        significant++;
    (void)sprintf(bound, "%lu", max);
    length = strlen(significant);
    if (length > strlen(bound) ||
        (length == strlen(bound) && strcmp(significant, bound) > 0))
        valid = 0;
    value = 123;
    actual = decimal(text, max, &value);
    assert(actual == valid);
    if (!actual)
        assert(value == 123);
    else {
        unsigned long expected = 0;
        for (i = 0; significant[i] != '\0'; i++)
            expected = expected * 10 + (unsigned long)(significant[i] - '0');
        assert(value == expected);
    }
    return 0;
}
