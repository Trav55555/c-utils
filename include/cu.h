/*
 * cu.h - safe, fast primitives for Dependable C (C89).
 *
 * Error policy:
 *  - Resource failures (out of memory, size overflow, malformed input) are
 *    returned: NULL from raw arena pushes, 0 from functions returning an ok
 *    flag. Failures are sticky on the arena and on builders, so a sequence of
 *    calls can be checked once at the end.
 *  - Contract violations (index out of bounds, temp scopes ended out of
 *    order, no free scratch arena, NULL where a value is required) are
 *    programmer errors and call abort().
 *
 * Memory policy: no per-object free. Memory lives in an arena until the arena
 * is released or a temp scope on it ends. Pushed memory is uninitialised;
 * initialise every field explicitly.
 */
#ifndef CU_H
#define CU_H

#include <stddef.h>

#if defined(__GNUC__) && !defined(__TINYC__)
#define CU_MUST_USE __attribute__((warn_unused_result))
#else
#define CU_MUST_USE
#endif

/* ---- Checked size arithmetic ------------------------------------------ */

/* Store a + b (a * b) in *out and return 1, or return 0 if it overflows
 * size_t. *out is unchanged on overflow. */
CU_MUST_USE int cu_size_add(size_t a, size_t b, size_t *out);
CU_MUST_USE int cu_size_mul(size_t a, size_t b, size_t *out);

/* ---- Arena ------------------------------------------------------------- */

typedef struct CuBlock CuBlock;

typedef struct CuArena {
    CuBlock *head;     /* newest block */
    size_t block_size; /* minimum size of a new block */
    int debug;         /* 1: one malloc per push, so sanitizers see each object */
    int failed;        /* sticky: some push failed */
} CuArena;

/* A position in an arena. Ending it frees everything pushed since. */
typedef struct CuTemp {
    CuArena *arena;
    CuBlock *block;
    size_t used;
} CuTemp;

/* Default block size 64 KiB. Debug mode is on by default when built with
 * AddressSanitizer or with CU_ARENA_DEBUG defined. */
void cu_arena_init(CuArena *a);
void cu_arena_init_with(CuArena *a, size_t block_size, int debug);

/* Frees every block. The arena can be pushed to again afterwards. */
void cu_arena_release(CuArena *a);

/* Uninitialised memory for count elements of elem_size bytes, aligned for any
 * type. Returns NULL and marks the arena failed if count * elem_size
 * overflows or memory runs out. A zero-size push returns a non-NULL pointer. */
CU_MUST_USE void *cu_arena_push(CuArena *a, size_t count, size_t elem_size);

/* Uninitialised bytes with no alignment. Same failure rules as cu_arena_push. */
CU_MUST_USE char *cu_arena_push_bytes(CuArena *a, size_t n);

#define CU_PUSH(a, T, n) ((T *)cu_arena_push((a), (n), sizeof(T)))

int cu_arena_failed(const CuArena *a);

/* Temp scopes must end in reverse order of beginning; ending one out of order
 * aborts. Pointers pushed inside a scope are invalid after it ends. */
CuTemp cu_temp_begin(CuArena *a);
void cu_temp_end(CuTemp t);

/* Per-thread scratch arenas. Returns a scope on a scratch arena that is not
 * in conflicts[0..count). Pass every arena the caller will allocate results
 * into, or scratch_end can free those results. Aborts if all scratch arenas
 * conflict. */
CuTemp cu_scratch_begin(CuArena **conflicts, size_t count);
/* The common case: one conflict, which may be NULL. */
CuTemp cu_scratch_begin1(CuArena *conflict);
void cu_scratch_end(CuTemp t);
/* Frees this thread's scratch arenas. Call before a thread exits. */
void cu_scratch_release(void);

/* ---- Strings ----------------------------------------------------------- */

/* A read-only view of len bytes. Not NUL-terminated. ptr is never NULL. */
typedef struct CuStr {
    const char *ptr;
    size_t len;
} CuStr;

/* A view of a string literal. A non-literal argument does not compile. */
#define CU_LIT(s) cu_str_n("" s "", sizeof("" s "") - 1)

/* ptr may be NULL only when len is 0. */
CuStr cu_str_n(const char *ptr, size_t len);
/* NULL gives the empty string. */
CuStr cu_str_cstr(const char *s);
CuStr cu_str_empty(void);

int cu_str_eq(CuStr a, CuStr b);
/* Aborts when i >= s.len. */
char cu_str_at(CuStr s, size_t i);

/* Slicing clamps to the string, so it never makes a view outside it. */
CuStr cu_str_prefix(CuStr s, size_t n);
CuStr cu_str_skip(CuStr s, size_t n);
/* Bytes [start, end), with end clamped to len and start clamped to end. */
CuStr cu_str_substr(CuStr s, size_t start, size_t end);
/* Without leading and trailing ASCII whitespace. */
CuStr cu_str_trim(CuStr s);

int cu_str_starts_with(CuStr s, CuStr prefix);
int cu_str_ends_with(CuStr s, CuStr suffix);
/* Returns 1 and stores the first match's offset in *at, or returns 0 and
 * leaves *at unchanged. The empty needle matches at 0. */
int cu_str_find(CuStr s, CuStr needle, size_t *at);
/* Splits around the first sep. If sep is absent, returns 0 with
 * *before = s and *after empty. */
int cu_str_cut(CuStr s, CuStr sep, CuStr *before, CuStr *after);

/* Copies into a. On failure returns the empty string and marks a failed. */
CuStr cu_str_copy(CuArena *a, CuStr s);
/* NUL-terminated copy for C APIs. Returns NULL on failure. */
CU_MUST_USE const char *cu_str_cstr_copy(CuArena *a, CuStr s);

/* ASCII classification. Safe for any char value, including negative ones;
 * bytes >= 0x80 are never letters, digits or space. Locale-independent. */
int cu_ascii_is_digit(char c);
int cu_ascii_is_alpha(char c);
int cu_ascii_is_space(char c);
char cu_ascii_to_lower(char c);

/* ---- Parsing ----------------------------------------------------------- */

/* The whole of s must be digits in base (2..36, either letter case): no
 * spaces, no "0x". Returns 0 on empty input, a bad digit or overflow, and
 * leaves *out unchanged. cu_parse_long also accepts one leading '+' or '-'.
 * A base outside 2..36 aborts. */
CU_MUST_USE int cu_parse_ulong(CuStr s, unsigned base, unsigned long *out);
CU_MUST_USE int cu_parse_long(CuStr s, unsigned base, long *out);

/* ---- String builder ---------------------------------------------------- */

/* Appends never fail loudly: a failure marks the builder, later appends are
 * skipped, and cu_sb_finish reports it. Memory comes from the arena. */
typedef struct CuBuilder {
    CuArena *arena;
    char *data;
    size_t len;
    size_t cap;
    int failed;
} CuBuilder;

void cu_sb_init(CuBuilder *b, CuArena *a);
void cu_sb_str(CuBuilder *b, CuStr s);
void cu_sb_char(CuBuilder *b, char c);
void cu_sb_ulong(CuBuilder *b, unsigned long v);
void cu_sb_long(CuBuilder *b, long v);
/* Lowercase hex, zero-padded to at least min_digits. */
void cu_sb_hex(CuBuilder *b, unsigned long v, size_t min_digits);
/* Stores the built string in *out and returns 1, or stores the empty string
 * and returns 0 if any append failed. */
CU_MUST_USE int cu_sb_finish(CuBuilder *b, CuStr *out);

#endif
