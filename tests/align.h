/* The arena promises memory aligned for any type. This measures the
 * strictest alignment a C89 object type needs (16 for x86_64's long
 * double, 4 on i386), from where a struct places each member after a
 * char; offsetof would do, but Cerberus cannot evaluate it. Include after
 * death.h. */
#ifndef CU_TESTS_ALIGN_H
#define CU_TESTS_ALIGN_H

#include <stddef.h>

static size_t max_alignment(void)
{
    struct { char c; long x; } l;
    struct { char c; double x; } d;
    struct { char c; long double x; } ld;
    struct { char c; void *x; } p;
    struct { char c; void (*x)(void); } f;
    size_t align[5];
    size_t most;
    size_t i;

    align[0] = (size_t)((char *)&l.x - &l.c);
    align[1] = (size_t)((char *)&d.x - &d.c);
    align[2] = (size_t)((char *)&ld.x - &ld.c);
    align[3] = (size_t)((char *)&p.x - &p.c);
    align[4] = (size_t)((char *)&f.x - &f.c);
    most = 1;
    for (i = 0; i < sizeof align / sizeof align[0]; i++) {
        if (align[i] > most)
            most = align[i];
    }
    return most;
}

static int is_max_aligned(const void *p)
{
    return (size_t)p % max_alignment() == 0;
}

#endif
