/*
 * cu.h - safe, fast primitives for Dependable C (C89).
 *
 * Error policy:
 *  - Resource failures (out of memory, size overflow, malformed input) are
 *    returned: NULL from raw arena pushes, 0 from functions returning an ok
 *    flag. Failures are sticky on the arena and on builders, so a sequence of
 *    calls can be checked once at the end.
 *  - Contract violations (index out of bounds, temp scopes ended out of
 *    order, no free scratch arena, NULL where a value is required, a
 *    container grown under the wrong temp scope) are programmer errors and
 *    call abort().
 *
 * Memory policy: no per-object free. Memory lives in an arena until the arena
 * is released or a temp scope on it ends. Pushed memory is uninitialised;
 * initialise every field explicitly.
 *
 * Containers (CuBuilder, CuVec, CuMap, CuPool, CuPlex) keep their memory in
 * the arena they are given, and remember the temp scope innermost on it
 * when they are initialised. A mutating call (append, push, reserve,
 * upsert, add, kind registration) while a different scope is innermost
 * aborts, whether or not that call would allocate: inside a newer scope,
 * ending it would free the container's memory under it; after its own scope
 * ended, that memory is already gone. Reading is allowed under newer scopes,
 * but only while the container's own scope is still open.
 *
 * Each release gives the arena a new base scope, so a container made
 * before it aborts when grown afterwards. Re-initialising an arena restarts
 * its ids instead, and a container or CuTemp from before that is not
 * caught: do not re-initialise an arena that still has users. */
#ifndef CU_H
#define CU_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__TINYC__)
#define CU_MUST_USE __attribute__((warn_unused_result))
#else
#define CU_MUST_USE
#endif

/* ---- Checked size arithmetic ------------------------------------------ */

/* Store a + b (a * b) in *out and return 1, or return 0 if it overflows
 * size_t. *out is unchanged on overflow. */
/** regions: borrows */
CU_MUST_USE int cu_size_add(size_t a, size_t b, size_t *out);
/** regions: borrows */
CU_MUST_USE int cu_size_mul(size_t a, size_t b, size_t *out);

/* ---- 64-bit unsigned integers ------------------------------------------ */

/* C89 has no 64-bit type, and long is 32 bits on Windows and wasm32, so a
 * CuU64 is two 32-bit halves: the value is hi * 2^32 + lo. Each half must
 * be at most 0xFFFFFFFF; every function aborts when given a half that is
 * not. Arithmetic is checked: overflow returns 0 and leaves *out
 * unchanged, as for cu_size_add. */
typedef struct CuU64 {
    unsigned long hi;
    unsigned long lo;
} CuU64;

/** regions: borrows */
CuU64 cu_u64(unsigned long hi, unsigned long lo);
/* Any unsigned long or size_t, including one wider than 32 bits. A value
 * that needs more than 64 bits aborts. */
/** regions: borrows */
CuU64 cu_u64_from_ulong(unsigned long v);
/** regions: borrows */
CuU64 cu_u64_from_size(size_t v);
/* Store v in *out and return 1, or return 0 if it does not fit size_t. */
/** regions: borrows */
CU_MUST_USE int cu_u64_to_size(CuU64 v, size_t *out);
/* Negative, zero or positive as a is less than, equal to or greater than b. */
/** regions: borrows */
int cu_u64_cmp(CuU64 a, CuU64 b);
/* a + b, failing past 2^64 - 1; a - b, failing when b > a. */
/** regions: borrows */
CU_MUST_USE int cu_u64_add(CuU64 a, CuU64 b, CuU64 *out);
/** regions: borrows */
CU_MUST_USE int cu_u64_sub(CuU64 a, CuU64 b, CuU64 *out);

/* ---- Arena ------------------------------------------------------------- */

typedef struct CuBlock CuBlock;

typedef struct CuArena {
    CuBlock *head;     /* newest block */
    size_t block_size; /* minimum size of a new block */
    int debug;         /* 1: one malloc per push, so sanitizers see each object */
    int failed;        /* sticky: some push failed */
    size_t blocks;     /* blocks ever made; numbers the next one, never reset */
    size_t held;       /* bytes in live blocks, headers included */
    size_t limit;      /* most bytes held + cached may reach; (size_t)-1: none */
    size_t refused;    /* bytes of the last block the limit refused, or 0 */
    size_t attempts;   /* block allocations tried, failed ones included;
                        * never reset */
    size_t fail_at;    /* the attempt that fails; (size_t)-1: none */
    size_t scope;      /* innermost open temp scope's id, else the base's */
    size_t scopes;     /* scope ids ever issued; numbers the next one */
    size_t mark_block; /* where the innermost open temp scope began: the */
    size_t mark_used;  /* newest block's number (0: none) and bytes used */
    CuBlock *cache;    /* a block freed by a scope's end, kept for reuse */
    size_t cached;     /* its bytes, header included, or 0; not in held */
} CuArena;

/* A position in an arena. Ending it frees everything pushed since. The block
 * is held by number, not address: a freed block's address can come back from
 * malloc, and a freed pointer must not even be compared. */
typedef struct CuTemp {
    CuArena *arena;
    size_t block;  /* number of the newest block, 0 if there was none */
    size_t used;
    size_t id;     /* never 0; repeats only after size_t wraps, or after
                    * the arena is re-initialised */
    size_t parent; /* the scope that was innermost when this one began */
    size_t parent_block; /* that scope's mark, restored when this one ends */
    size_t parent_used;
} CuTemp;

/* Default block size 64 KiB. Debug mode is on by default when built with
 * AddressSanitizer or with CU_ARENA_DEBUG defined. */
/** regions: borrows */
void cu_arena_init(CuArena *a);
/** regions: borrows */
void cu_arena_init_with(CuArena *a, size_t block_size, int debug);

/* Frees every block, the cached one included. The arena can be pushed to
 * again afterwards, but temp scopes and containers from before the release
 * are dead: ending or growing one aborts. */
/** regions: borrows */
void cu_arena_release(CuArena *a);

/* Uninitialised memory for count elements of elem_size bytes, aligned for any
 * type. Returns NULL and marks the arena failed if count * elem_size
 * overflows or memory runs out. A zero-size push returns a non-NULL pointer. */
/** regions: returns alloc(a) */
CU_MUST_USE void *cu_arena_push(CuArena *a, size_t count, size_t elem_size);

/* Uninitialised bytes with no alignment. Same failure rules as cu_arena_push. */
/** regions: returns alloc(a) */
CU_MUST_USE char *cu_arena_push_bytes(CuArena *a, size_t n);

#define CU_PUSH(a, T, n) ((T *)cu_arena_push((a), (n), sizeof(T)))

/* Grows p, an allocation of old_size bytes from a, to new_size bytes and
 * returns where it now is; its first old_size bytes are kept. p may be NULL
 * only when old_size is 0, and then this is cu_arena_push(a, new_size, 1).
 * A new_size of at most old_size returns p unchanged.
 *
 * When p is the newest allocation, was pushed under the innermost temp scope
 * (or with none open), and new_size fits in the rest of its block, p grows
 * in place: no block allocation, held unchanged. When p is all there is in
 * the newest block (it starts at the block's beginning) and that block was
 * made under the innermost scope, the block is reallocated to fit: one
 * block allocation attempt (cu_arena_fail_at), held grows by the bytes added, the limit applies to those bytes, and a
 * refusal records them in refused. Otherwise this is cu_arena_push(a,
 * new_size, 1) and a copy, and p's old bytes stay held until their scope
 * ends or the arena is released.
 *
 * A successful call may free p's old memory, as realloc does: p, every
 * pointer into its bytes, and any zero-size allocation at p's address are
 * invalid unless p is returned, so copy anything still needed from them
 * first. Pointers to other allocations are unaffected. On failure it
 * returns NULL, marks the arena failed, and p keeps its bytes. Memory from
 * before the innermost temp scope is never grown in place or reallocated
 * (nor is the block that scope began in), so ending the scope frees exactly
 * what was pushed in it. old_size 0 always gets a new push. */
/** regions: returns alloc(a); requires p in a */
CU_MUST_USE void *cu_arena_grow(CuArena *a, void *p, size_t old_size, size_t new_size);

/* Grows p (old_size bytes from a, not NULL) to new_size bytes in place, as
 * cu_arena_grow does in its first case, and returns 1; or returns 0,
 * changing nothing, when that is not possible. It never allocates, moves
 * or frees. A new_size of at most old_size returns 1; old_size 0 returns
 * 0. CuBuilder grows this way or with a new push, never by realloc. */
/** regions: borrows */
CU_MUST_USE int cu_arena_extend(CuArena *a, void *p, size_t old_size, size_t new_size);

/** regions: borrows */
int cu_arena_failed(const CuArena *a);

/* Caps the bytes the arena's blocks may hold, headers included, the cached
 * block (see cu_temp_end) counted too: held + cached never passes it. A
 * push that would pass the cap fails exactly as if memory ran out, and
 * records the block's size in refused: held + refused is the smallest cap
 * under which that push succeeds, since the cached block is dropped before
 * a new block is allocated. Releasing the arena or ending a temp scope
 * frees bytes for later pushes. A cap below held + cached drops the cached
 * block. The default, (size_t)-1, means no cap. Use it to bound memory
 * spent on untrusted input, and in tests to make each allocation fail in
 * turn. */
/** regions: borrows */
void cu_arena_set_limit(CuArena *a, size_t max_bytes);

/* Test hook: the block allocation numbered n (counting attempts from 0 in
 * a.attempts) fails exactly as if memory ran out, and every other one
 * succeeds. Only a push that needs a new block, or a cu_arena_grow that
 * reallocates one, is an attempt; in debug mode every push is. Tests step
 * n from 0 until a run makes no more than n attempts, so each allocation
 * fails alone; a cap would also fail the allocations after it, which can
 * hide a failure the code ignores. (size_t)-1, the default, fails none. */
/** regions: borrows */
void cu_arena_fail_at(CuArena *a, size_t n);

/* Temp scopes must end in reverse order of beginning, each exactly once;
 * ending any scope but the innermost one aborts. Releasing the arena discards
 * its open scopes, so ending one afterwards aborts too. Pointers pushed
 * inside a scope are invalid after it ends.
 *
 * Ending a scope frees the blocks made in it, except that the largest is
 * kept as the arena's cached block, replacing a smaller one, when not in
 * debug mode. The next push that needs a new block takes the cached block
 * if it is big enough (it counts as an attempt for cu_arena_fail_at, and
 * its bytes move from cached to held); otherwise the cached block is freed
 * and a new one allocated. So a scope that is opened and filled again and
 * again reuses its memory instead of going back to malloc each time.
 * Releasing the arena frees the cached block too. */
/** regions: opens scope(a) */
CuTemp cu_temp_begin(CuArena *a);
/** regions: closes scope(t) */
void cu_temp_end(CuTemp t);
/* The id of the innermost temp scope open on a or, with none open, of the
 * arena's base scope, which each release replaces. Never 0. */
/** regions: borrows */
size_t cu_arena_scope(const CuArena *a);

/* Per-thread scratch arenas. Returns a scope on a scratch arena that is not
 * in conflicts[0..count). Pass every arena the caller will allocate results
 * into, or scratch_end can free those results. Aborts if all scratch arenas
 * conflict. */
/** regions: opens scratch(conflicts, count) */
CuTemp cu_scratch_begin(CuArena **conflicts, size_t count);
/* The common case: one conflict, which may be NULL. */
/** regions: opens scratch1(conflict) */
CuTemp cu_scratch_begin1(CuArena *conflict);
/** regions: closes scope(t) */
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
/** regions: returns view(ptr) */
CuStr cu_str_n(const char *ptr, size_t len);
/* NULL gives the empty string. */
/** regions: returns view(s) */
CuStr cu_str_cstr(const char *s);
/** regions: returns static */
CuStr cu_str_empty(void);

/** regions: borrows */
int cu_str_eq(CuStr a, CuStr b);
/* Aborts when i >= s.len. */
/** regions: borrows */
char cu_str_at(CuStr s, size_t i);
/* cu_str_at without a call when i is in bounds; out of bounds it calls
 * cu_str_at, which aborts. Evaluates s and i more than once, so pass
 * expressions without side effects. */
#define CU_STR_AT_FAST(s, i) ((size_t)(i) < (s).len ? (s).ptr[(size_t)(i)] : cu_str_at((s), (i)))

/* Slicing clamps to the string, so it never makes a view outside it. */
/** regions: returns view(s) */
CuStr cu_str_prefix(CuStr s, size_t n);
/** regions: returns view(s) */
CuStr cu_str_skip(CuStr s, size_t n);
/* Bytes [start, end), with end clamped to len and start clamped to end. */
/** regions: returns view(s) */
CuStr cu_str_substr(CuStr s, size_t start, size_t end);
/* Without leading and trailing ASCII whitespace. */
/** regions: returns view(s) */
CuStr cu_str_trim(CuStr s);

/** regions: borrows */
int cu_str_starts_with(CuStr s, CuStr prefix);
/** regions: borrows */
int cu_str_ends_with(CuStr s, CuStr suffix);
/* Returns 1 and stores the first match's offset in *at, or returns 0 and
 * leaves *at unchanged. The empty needle matches at 0. Linear in s.len +
 * needle.len on any input: memchr and memcmp on ordinary text, switching to
 * Two-Way matching when candidates for the first byte come too often. */
/** regions: borrows */
int cu_str_find(CuStr s, CuStr needle, size_t *at);
/* Splits around the first sep. If sep is absent, returns 0 with
 * *before = s and *after empty. */
/** regions: writes *before view(s); writes *after view(s) */
int cu_str_cut(CuStr s, CuStr sep, CuStr *before, CuStr *after);

/* Copies into a. On failure returns the empty string and marks a failed, so
 * check cu_arena_failed when an empty result is possible. */
/** regions: returns alloc(a) */
CU_MUST_USE CuStr cu_str_copy(CuArena *a, CuStr s);
/* NUL-terminated copy for C APIs. Returns NULL on failure. */
/** regions: returns alloc(a) */
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
/** regions: borrows */
CU_MUST_USE int cu_parse_ulong(CuStr s, unsigned base, unsigned long *out);
/** regions: borrows */
CU_MUST_USE int cu_parse_long(CuStr s, unsigned base, long *out);

/* ---- String builder ---------------------------------------------------- */

/* Appends never fail loudly: a failure marks the builder, later appends are
 * skipped, and cu_sb_finish reports it. Memory comes from the arena; append
 * only under the temp scope the builder was initialised in. */
typedef struct CuBuilder {
    CuArena *arena;
    char *data;
    size_t len;
    size_t cap;
    size_t scope;
    int failed;
    int shared; /* a view from cu_sb_finish may point into data */
} CuBuilder;

/** regions: binds b to a */
void cu_sb_init(CuBuilder *b, CuArena *a);
/** regions: allocates in b */
void cu_sb_str(CuBuilder *b, CuStr s);
/** regions: allocates in b */
void cu_sb_char(CuBuilder *b, char c);
/** regions: allocates in b */
void cu_sb_ulong(CuBuilder *b, unsigned long v);
/** regions: allocates in b */
void cu_sb_long(CuBuilder *b, long v);
/* Lowercase hex, zero-padded to at least min_digits. */
/** regions: allocates in b */
void cu_sb_hex(CuBuilder *b, unsigned long v, size_t min_digits);
/* Stores the built string in *out and returns 1, or stores the empty string
 * and returns 0 if any append failed. The view stays valid while the
 * builder keeps growing (until its scope ends or the arena is released):
 * after a finish, a builder grows in place or into a new buffer, leaving
 * the old bytes where they were, so it can also append its own finished
 * contents. Until the first finish, and again after such a move, growth
 * may reallocate (cu_arena_grow), so a pointer taken from the data field
 * is valid only until the next append. */
/** regions: writes *out view(*b) */
CU_MUST_USE int cu_sb_finish(CuBuilder *b, CuStr *out);

/* ---- Binary reading and writing ------------------------------------- */

/* Fixed-width unsigned integers in little (le) or big (be) endian order,
 * read from s at *pos. On success each reader stores the value in *out,
 * advances *pos past the bytes read, and returns 1. If fewer bytes remain,
 * or *pos > s.len, it returns 0 and leaves *pos and *out unchanged. Bytes
 * are assembled arithmetically, so input alignment and host byte order do
 * not matter. */
/** regions: borrows */
CU_MUST_USE int cu_rd_u8(CuStr s, size_t *pos, unsigned long *out);
/** regions: borrows */
CU_MUST_USE int cu_rd_u16le(CuStr s, size_t *pos, unsigned long *out);
/** regions: borrows */
CU_MUST_USE int cu_rd_u16be(CuStr s, size_t *pos, unsigned long *out);
/** regions: borrows */
CU_MUST_USE int cu_rd_u32le(CuStr s, size_t *pos, unsigned long *out);
/** regions: borrows */
CU_MUST_USE int cu_rd_u32be(CuStr s, size_t *pos, unsigned long *out);
/* 64-bit readers store a CuU64 under the same rules. */
/** regions: borrows */
CU_MUST_USE int cu_rd_u64le(CuStr s, size_t *pos, CuU64 *out);
/** regions: borrows */
CU_MUST_USE int cu_rd_u64be(CuStr s, size_t *pos, CuU64 *out);
/* The next n bytes as a view into s, under the same rules. */
/** regions: writes *out view(s) */
CU_MUST_USE int cu_rd_bytes(CuStr s, size_t *pos, size_t n, CuStr *out);

/* Append v in the given width and byte order. A v that does not fit the
 * width aborts. Failure is sticky, as for every builder append. */
/** regions: allocates in b */
void cu_sb_u8(CuBuilder *b, unsigned long v);
/** regions: allocates in b */
void cu_sb_u16le(CuBuilder *b, unsigned long v);
/** regions: allocates in b */
void cu_sb_u16be(CuBuilder *b, unsigned long v);
/** regions: allocates in b */
void cu_sb_u32le(CuBuilder *b, unsigned long v);
/** regions: allocates in b */
void cu_sb_u32be(CuBuilder *b, unsigned long v);
/* A CuU64 with a half over 0xFFFFFFFF aborts. */
/** regions: allocates in b */
void cu_sb_u64le(CuBuilder *b, CuU64 v);
/** regions: allocates in b */
void cu_sb_u64be(CuBuilder *b, CuU64 v);

/* ---- SHA-256 ----------------------------------------------------------- */

/* FIPS 180-4 SHA-256 for content addressing. Not hardened against timing
 * side channels: hash public content, not secrets, and do not build MACs
 * or password hashes on it. Feed any number of updates between init and
 * final; final writes the 32-byte digest and finishes the hasher, and a
 * further update or final aborts until the next init. A message of 2^61
 * bytes or more aborts, since its bit length would not fit 64 bits. */
typedef struct CuSha256 {
    unsigned long state[8];
    CuU64 len; /* bytes hashed so far */
    unsigned char block[64];
    size_t used; /* bytes waiting in block */
    int done;
} CuSha256;

/** regions: borrows */
void cu_sha256_init(CuSha256 *h);
/** regions: borrows */
void cu_sha256_update(CuSha256 *h, CuStr data);
/** regions: borrows */
void cu_sha256_final(CuSha256 *h, unsigned char digest[32]);
/* init, one update and final. */
/** regions: borrows */
void cu_sha256(CuStr data, unsigned char digest[32]);

/* ---- SHA-1 ------------------------------------------------------------- */

/* FIPS 180-4 SHA-1, for the object ids git uses. SHA-1 is broken for
 * collision resistance, and this has none of the collision detection git
 * adds: use it to name objects as git does, never to trust that equal
 * digests mean equal content. Same contract as SHA-256 above: updates
 * between init and final, final finishes the hasher, misuse aborts, and a
 * message of 2^61 bytes or more aborts. */
typedef struct CuSha1 {
    unsigned long state[5];
    CuU64 len; /* bytes hashed so far */
    unsigned char block[64];
    size_t used; /* bytes waiting in block */
    int done;
} CuSha1;

/** regions: borrows */
void cu_sha1_init(CuSha1 *h);
/** regions: borrows */
void cu_sha1_update(CuSha1 *h, CuStr data);
/** regions: borrows */
void cu_sha1_final(CuSha1 *h, unsigned char digest[20]);
/* init, one update and final. */
/** regions: borrows */
void cu_sha1(CuStr data, unsigned char digest[20]);

/* ---- Growable array ---------------------------------------------------- */

/* Elements of one type, stored in an arena. Use the CU_VEC_* macros: each
 * passes sizeof the element type at the call site, and a size that does not
 * match the vector's element size aborts, so most type mix-ups are caught.
 * Pushes never fail loudly: a failure marks the vector, the push is skipped,
 * and cu_vec_failed reports it. Growth moves the elements and may free the
 * old buffer (cu_arena_grow), so pointers from CU_VEC_AT and CU_VEC_DATA,
 * and the data field, are valid only until the next push or reserve.
 * Pushing a pointer to one of the vector's own elements is allowed. Push
 * and reserve only under the temp scope the vector was initialised in. */
typedef struct CuVec {
    CuArena *arena;
    char *data;
    size_t len;
    size_t cap;
    size_t elem_size;
    size_t scope;
    int failed;
} CuVec;

#define CU_VEC_INIT(v, a, T) cu_vec_init((v), (a), sizeof(T))
/* Copies *p onto the end. */
#define CU_VEC_PUSH(v, p) cu_vec_push_((v), (p), sizeof *(p))
/* Copies the last element into *p and removes it; returns 0 if empty. */
#define CU_VEC_POP(v, p) cu_vec_pop_((v), (p), sizeof *(p))
/* Pointer to element i; aborts when i >= len. */
#define CU_VEC_AT(v, T, i) ((T *)cu_vec_at_((v), (i), sizeof(T)))
/* Pointer to the first element, for loops over 0..len. Never NULL. */
#define CU_VEC_DATA(v, T) ((T *)cu_vec_data_((v), sizeof(T)))

/* CU_VEC_PUSH and CU_VEC_AT with their common case inline: every check the
 * function makes is made first (NULL, element size, bounds; for a push also
 * the temp scope, failure and spare capacity), and anything else calls the
 * function, which aborts, grows or skips exactly as it would. They evaluate
 * their arguments more than once, so pass expressions without side effects.
 * A push copies through memcpy, which is why cu.h includes <string.h>. */
#define CU_VEC_PUSH_FAST(v, p)                                                          \
    ((v) != NULL && (p) != NULL && sizeof *(p) == (v)->elem_size && !(v)->failed &&     \
             (v)->len < (v)->cap && (v)->arena->scope == (v)->scope                      \
         ? (void)(memcpy((v)->data + (v)->len * sizeof *(p), (p), sizeof *(p)), (v)->len++) \
         : cu_vec_push_((v), (p), sizeof *(p)))
#define CU_VEC_AT_FAST(v, T, i)                                                         \
    ((v) != NULL && sizeof(T) == (v)->elem_size && (size_t)(i) < (v)->len               \
         ? (T *)(void *)((v)->data + (size_t)(i) * sizeof(T))                           \
         : CU_VEC_AT((v), T, (i)))

/* elem_size 0 aborts. */
/** regions: binds v to a */
void cu_vec_init(CuVec *v, CuArena *a, size_t elem_size);
/* Room for at least extra more elements without moving. Returns 0 and marks
 * the vector failed on overflow or out of memory; once failed, returns 0
 * without allocating. */
/** regions: allocates in v */
int cu_vec_reserve(CuVec *v, size_t extra);
/** regions: borrows */
size_t cu_vec_len(const CuVec *v);
/** regions: borrows */
int cu_vec_failed(const CuVec *v);
/* Sets len to 0 and keeps the storage. */
/** regions: borrows */
void cu_vec_clear(CuVec *v);

/** regions: allocates in v; stores *elem in *v */
void cu_vec_push_(CuVec *v, const void *elem, size_t elem_size);
/** regions: writes *out view(*v) */
int cu_vec_pop_(CuVec *v, void *out, size_t elem_size);
/** regions: returns view(*v) */
void *cu_vec_at_(const CuVec *v, size_t i, size_t elem_size);
/** regions: returns view(*v) */
void *cu_vec_data_(const CuVec *v, size_t elem_size);

/* ---- Slices ------------------------------------------------------------ */

/* A view of len elements of elem_size bytes, owned elsewhere; the elements
 * can be written through it. As with CuVec, use the CU_SLICE_* macros: a
 * size that does not match the slice's element size aborts. ptr is never
 * NULL. A slice of a CuVec is valid only until the vector's next push or
 * reserve. */
typedef struct CuSlice {
    char *ptr;
    size_t len;
    size_t elem_size;
} CuSlice;

/* The n elements starting at p, typed by p. p may be NULL only when n is 0;
 * a size n * sizeof *p that overflows aborts. */
#define CU_SLICE(p, n) cu_slice_n_((p), (n), sizeof *(p))
/* Pointer to element i; aborts when i >= len. */
#define CU_SLICE_AT(s, T, i) ((T *)cu_slice_at_((s), (i), sizeof(T)))
/* Pointer to the first element, for loops over 0..len. Never NULL. */
#define CU_SLICE_DATA(s, T) ((T *)cu_slice_data_((s), sizeof(T)))

/** regions: returns view(*v) */
CuSlice cu_vec_slice(const CuVec *v);
/* Elements [start, end), with end clamped to len and start clamped to end,
 * so it never makes a view outside s. */
/** regions: returns view(s) */
CuSlice cu_slice_sub(CuSlice s, size_t start, size_t end);

/** regions: returns view(p) */
CuSlice cu_slice_n_(void *p, size_t n, size_t elem_size);
/** regions: returns view(s) */
void *cu_slice_at_(CuSlice s, size_t i, size_t elem_size);
/** regions: returns view(s) */
void *cu_slice_data_(CuSlice s, size_t elem_size);

/* ---- Hash map ---------------------------------------------------------- */

/* 32-bit FNV-1a of s's bytes. */
/** regions: borrows */
unsigned long cu_hash_fnv1a(CuStr s);

/* A hash trie (Wellons' design) from CuStr keys to values of one type, in
 * an arena. Each node has four children chosen by successive 2-bit slices
 * of the key's hash: nothing is rehashed, entries never move, and nothing
 * is freed individually. Keys are copied into the arena. There is no
 * removal. Value pointers stay valid until the arena is released or the
 * temp scope the map was initialised in ends; upsert only under that scope.
 *
 * Cost per call is the trie's depth, logarithmic for ordinary keys. Keys
 * whose 32-bit hashes are equal share one path that ends in a list, so n
 * such keys cost O(n) each; FNV-1a collisions are easy to construct, so
 * bound the number of keys taken from untrusted input (cu_arena_set_limit
 * on the map's arena caps them by memory).
 *
 * As with CuVec, use the CU_MAP_* macros: a size that does not match the
 * map's value size aborts. A failed insertion marks the map; after that no
 * new key is inserted, existing keys are still found, and cu_map_failed
 * reports it. */
typedef struct CuMapNode CuMapNode;

typedef struct CuMap {
    CuArena *arena;
    CuMapNode *root;
    CuMapNode *first; /* insertion order, for iteration */
    CuMapNode *last;
    size_t len;
    size_t value_size;
    size_t scope;
    int failed;
} CuMap;

typedef struct CuMapIter {
    CuMapNode *node;
    size_t value_size;
} CuMapIter;

#define CU_MAP_INIT(m, a, T) cu_map_init((m), (a), sizeof(T))
/* Pointer to key's value. A new key is first inserted with a copy of *p.
 * Returns NULL if a new key cannot be inserted. */
#define CU_MAP_UPSERT(m, T, key, p) ((T *)cu_map_upsert_((m), (key), (p), sizeof *(p), sizeof(T)))
/* Pointer to key's value, or NULL if key is absent. */
#define CU_MAP_GET(m, T, key) ((T *)cu_map_get_((m), (key), sizeof(T)))
/* The next entry in insertion order: stores its key in *key and returns a
 * pointer to its value, or returns NULL at the end. */
#define CU_MAP_NEXT(it, T, key) ((T *)cu_map_next_((it), (key), sizeof(T)))

/* value_size 0 aborts. */
/** regions: binds m to a */
void cu_map_init(CuMap *m, CuArena *a, size_t value_size);
/** regions: borrows */
size_t cu_map_len(const CuMap *m);
/** regions: borrows */
int cu_map_failed(const CuMap *m);
/** regions: returns view(*m) */
CuMapIter cu_map_iter(const CuMap *m);

/** regions: allocates in m; stores *init in *m; returns view(*m) */
void *cu_map_upsert_(CuMap *m, CuStr key, const void *init, size_t init_size, size_t value_size);
/** regions: returns view(*m) */
void *cu_map_get_(const CuMap *m, CuStr key, size_t value_size);
/** regions: returns view(*it); writes *key view(*it) */
void *cu_map_next_(CuMapIter *it, CuStr *key, size_t value_size);

/* ---- Pool with generational handles ------------------------------------ */

/* A handle names one object in a pool. Generation 0 is the null handle,
 * which never names an object. */
typedef struct CuHandle {
    size_t index;
    unsigned long gen;
} CuHandle;

/* Objects of one type that can be removed individually, named by handles
 * rather than pointers. Once an object is removed every handle to it is
 * stale: CU_POOL_GET returns NULL and cu_pool_remove returns 0. A stale,
 * null or forged handle is reported this way and never dereferenced. A
 * freed slot is reused under the next generation, so old handles stay
 * stale; a slot that has reached max_gen is retired instead, so a
 * generation never wraps around to a value an old handle still holds.
 *
 * Slots live in fixed-size pages in the arena: an object's pointer stays
 * valid until the object is removed, and the pool lives until the arena is
 * released or the temp scope it was initialised in ends; add only under
 * that scope. As with CuVec, a size that does not match the pool's value size
 * aborts. A failed addition (out of memory) marks the pool and returns the
 * null handle; later additions also fail, and cu_pool_failed reports it. */
typedef struct CuPool {
    CuArena *arena;
    CuVec pages;
    size_t slots;     /* slots ever made */
    size_t free_head; /* first reusable slot, or (size_t)-1 */
    size_t len;       /* live objects */
    size_t value_size;
    size_t stride;    /* bytes per slot, header and value; 0: overflowed */
    unsigned long max_gen;
    size_t scope;
    int failed;
} CuPool;

#define CU_POOL_INIT(p, a, T) cu_pool_init((p), (a), sizeof(T))
/* Adds a copy of *v; returns its handle, or the null handle on failure. */
#define CU_POOL_ADD(p, v) cu_pool_add_((p), (v), sizeof *(v))
/* Pointer to h's object, or NULL if h is stale, null or forged. */
#define CU_POOL_GET(p, T, h) ((T *)cu_pool_get_((p), (h), sizeof(T)))

/* Generations up to the largest unsigned long. value_size 0 aborts. */
/** regions: binds p to a */
void cu_pool_init(CuPool *p, CuArena *a, size_t value_size);
/* Generations up to max_gen, which must be at least 1. */
/** regions: binds p to a */
void cu_pool_init_with(CuPool *p, CuArena *a, size_t value_size, unsigned long max_gen);
/* Removes h's object. Returns 0, changing nothing, if h is stale, null or
 * forged. */
/** regions: borrows */
int cu_pool_remove(CuPool *p, CuHandle h);
/** regions: borrows */
size_t cu_pool_len(const CuPool *p);
/** regions: borrows */
int cu_pool_failed(const CuPool *p);
CuHandle cu_handle_null(void);
int cu_handle_is_null(CuHandle h);

/** regions: allocates in p; stores *v in *p */
CU_MUST_USE CuHandle cu_pool_add_(CuPool *p, const void *v, size_t value_size);
/** regions: returns view(*p) */
void *cu_pool_get_(const CuPool *p, CuHandle h, size_t value_size);

/* ---- Plex -------------------------------------------------------------- */

/* A link from one node to another. kind 0 is the null link. */
typedef struct CuLink {
    unsigned kind;
    CuHandle h;
} CuLink;

/* Nodes of several kinds that link to each other in any shape, cycles and
 * back links included (a plex, in Ross's and Knuth's sense). Each kind is a
 * CuPool of one node type; nodes hold CuLinks, never pointers, to other
 * nodes. Following a link that is null, stale (its node was removed) or of
 * another kind returns NULL: a bad link is reported, never followed, and
 * removing a node needs no unlinking.
 *
 * Kinds are registered with CU_PLEX_KIND and numbered from 1. A kind passed
 * by the caller that was never registered, or a type whose size differs
 * from its kind's node size, aborts; a link carrying an unknown kind is
 * data and only gets NULL. Node pointers stay valid until the node is
 * removed. Register kinds and add nodes only under the temp scope the plex
 * was initialised in. A failed registration or addition (out of memory) marks the
 * plex; later ones also fail, and cu_plex_failed reports it. */
typedef struct CuPlex {
    CuArena *arena;
    CuVec kinds; /* CuPool per kind; kind k is element k - 1 */
    int failed;
} CuPlex;

/* Registers a node kind of type T; returns its id, or 0 on failure. */
#define CU_PLEX_KIND(px, T) cu_plex_kind((px), sizeof(T))
/* Adds a copy of *v as a node of kind; returns a link to it, or the null
 * link on failure. */
#define CU_PLEX_ADD(px, kind, v) cu_plex_add_((px), (kind), (v), sizeof *(v))
/* Pointer to the node of kind that link names, or NULL. */
#define CU_PLEX_GET(px, T, kind, link) ((T *)cu_plex_get_((px), (kind), (link), sizeof(T)))

/** regions: binds px to a */
void cu_plex_init(CuPlex *px, CuArena *a);
/** regions: allocates in px */
CU_MUST_USE unsigned cu_plex_kind(CuPlex *px, size_t node_size);
/* Removes link's node. Returns 0, changing nothing, if link is null, stale
 * or names an unknown kind. */
/** regions: borrows */
int cu_plex_remove(CuPlex *px, CuLink link);
/** regions: borrows */
int cu_plex_failed(const CuPlex *px);
CuLink cu_link_null(void);
int cu_link_is_null(CuLink link);
/* Whether a and b name the same node; a stale link still equals itself. */
int cu_link_eq(CuLink a, CuLink b);

/** regions: allocates in px; stores *v in *px */
CU_MUST_USE CuLink cu_plex_add_(CuPlex *px, unsigned kind, const void *v, size_t node_size);
/** regions: returns view(*px) */
void *cu_plex_get_(const CuPlex *px, unsigned kind, CuLink link, size_t node_size);

/* ---- JSON -------------------------------------------------------------
 *
 * RFC 8259, strictly. cu_json_parse reads one value into a tree in an
 * arena, or fails with the offset and reason. It accepts exactly the JSON
 * grammar: no comments, trailing commas, leading zeros, NaN or bytes after
 * the value. Strings must be valid UTF-8 (no overlong forms, surrogates or
 * raw control characters), \u escapes must pair their surrogates, and an
 * object's keys must differ. Nesting is limited to CU_JSON_MAX_DEPTH
 * arrays and objects.
 *
 * Memory: each value takes a CuJson, each object member a CuStr and a
 * size_t more, and each string and number its decoded bytes, all sized
 * exactly. The shortest value is one byte and a comma, so on a 64-bit
 * machine the tree can take up to about 32 times the input (single digits
 * in arrays, with alignment); typical API bodies take a few times. Untrusted input needs a
 * cap on its size and cu_arena_set_limit on a, which fails the parse with
 * "out of memory" cleanly. Time is linear in the input times its nesting
 * depth, which CU_JSON_MAX_DEPTH bounds.
 *
 * Numbers keep their text, since JSON does not say how wide they are;
 * cu_json_to_u64 and cu_json_to_long read the integers that fit. Object
 * lookup is a binary search over the keys' byte order.
 *
 * CuJsonOut writes JSON into its own builder. Strings are escaped as JSON
 * requires; one that is not valid UTF-8, a number whose text is not a
 * JSON number, or a key repeated in one object marks the writer failed and
 * finish reports it, so it writes only JSON that cu_json_parse accepts. It
 * also records where each open object's keys start, in the same arena, so
 * the builder may grow by copying rather than in place. Calls out of
 * order (a value where a key belongs, a second top-level value, an end
 * that does not match, nesting past CU_JSON_MAX_DEPTH, finishing early)
 * abort. */
#define CU_JSON_MAX_DEPTH 64

typedef enum CuJsonKind {
    CU_JSON_NULL,
    CU_JSON_BOOL,
    CU_JSON_NUMBER,
    CU_JSON_STRING,
    CU_JSON_ARRAY,
    CU_JSON_OBJECT
} CuJsonKind;

typedef struct CuJson {
    CuJsonKind kind;
    int truth;            /* BOOL: 0 or 1 */
    CuStr text;           /* STRING: the decoded bytes; NUMBER: its text */
    struct CuJson *items; /* ARRAY: the elements; OBJECT: the values */
    CuStr *keys;          /* OBJECT: the keys, in input order */
    size_t *order;        /* OBJECT: indices of the keys in byte order */
    size_t n;             /* ARRAY, OBJECT: how many */
} CuJson;

typedef struct CuJsonError {
    size_t offset;      /* where in the input parsing stopped */
    const char *reason; /* a static string */
} CuJsonError;

/* Parses src into *out, everything allocated in a. Returns 1, or 0 with
 * *err set (out of memory is a reason too). */
/** regions: writes *out alloc(a) */
CU_MUST_USE int cu_json_parse(CuArena *a, CuStr src, CuJson *out, CuJsonError *err);
/* The value of key in obj, or NULL if obj has no such key. obj must be an
 * object. */
/** regions: returns view(*obj) */
const CuJson *cu_json_find(const CuJson *obj, CuStr key);
/* A number with no sign, fraction or exponent that fits: stores it and
 * returns 1, else returns 0 and leaves *out unchanged. */
/** regions: borrows */
CU_MUST_USE int cu_json_to_u64(const CuJson *v, CuU64 *out);
/* The same for an integer that fits long, which may be negative. */
/** regions: borrows */
CU_MUST_USE int cu_json_to_long(const CuJson *v, long *out);

typedef struct CuJsonOut {
    CuBuilder b;
    CuVec keys;                              /* offsets in b of open objects' keys */
    size_t first[CU_JSON_MAX_DEPTH];         /* keys' length when that level opened */
    size_t depth;                            /* containers open */
    unsigned char object[CU_JSON_MAX_DEPTH]; /* 1: that level is an object */
    unsigned char items[CU_JSON_MAX_DEPTH];  /* 1: that level has an item */
    int want_value;                          /* a key was written */
    int done;                                /* the top-level value is complete */
    int failed;
} CuJsonOut;

/** regions: binds o to a */
void cu_json_out_init(CuJsonOut *o, CuArena *a);
/** regions: allocates in o */
void cu_json_begin_object(CuJsonOut *o);
/** regions: allocates in o */
void cu_json_end_object(CuJsonOut *o);
/** regions: allocates in o */
void cu_json_begin_array(CuJsonOut *o);
/** regions: allocates in o */
void cu_json_end_array(CuJsonOut *o);
/** regions: allocates in o */
void cu_json_key(CuJsonOut *o, CuStr key);
/** regions: allocates in o */
void cu_json_string(CuJsonOut *o, CuStr s);
/** regions: allocates in o */
void cu_json_u64(CuJsonOut *o, CuU64 v);
/** regions: allocates in o */
void cu_json_long(CuJsonOut *o, long v);
/* text must be a JSON number, as from a parsed CU_JSON_NUMBER; a NULL
 * text.ptr with a nonzero text.len aborts. */
/** regions: allocates in o */
void cu_json_number(CuJsonOut *o, CuStr text);
/** regions: allocates in o */
void cu_json_bool(CuJsonOut *o, int v);
/** regions: allocates in o */
void cu_json_null(CuJsonOut *o);
/* Stores the JSON in *out and returns 1; or stores the empty string and
 * returns 0 if the writer failed or ran out of memory. Every container
 * must be closed. The view lasts as cu_sb_finish's does. */
/** regions: writes *out view(*o) */
CU_MUST_USE int cu_json_out_finish(CuJsonOut *o, CuStr *out);

/* ---- Base64 -----------------------------------------------------------
 *
 * The standard alphabet (RFC 4648 section 4) with padding. Decoding is
 * strict, so each byte string has one accepted text: the length is a
 * multiple of 4, '=' appears only as the final one or two characters, the
 * bits padding leaves over are zero, and nothing else (no whitespace, no
 * URL-safe letters) is accepted. */

/** regions: allocates in b */
void cu_sb_base64(CuBuilder *b, CuStr bytes);
/* Decodes text into *out, allocated in a. Returns 1, or 0 with *out empty
 * if text is not canonical base64 or a runs out of memory. */
/** regions: writes *out alloc(a) */
CU_MUST_USE int cu_base64_decode(CuArena *a, CuStr text, CuStr *out);

/* ---- Files: the platform module's first part ---------------------------
 *
 * Whole-file reads into an arena. The bytes are anything (NULs included)
 * and live as long as the arena's current scope; *out views them, and is
 * the empty string on failure. A read fails (returns 0) if the file cannot
 * be opened or read, with errno supplied by stdio, or if memory runs out,
 * with the arena's sticky failure flag set. Cleanup preserves an earlier
 * failure's errno. Buffers remain arena-owned until scope end or release,
 * including on failure. NULL arguments abort. */

/* Reads the file at path and always closes it. A nonempty file needs one
 * arena push of its size plus one byte when the size can be found and
 * stays unchanged; a growing file is still read to its end. Empty files
 * need no allocation. */
/** regions: writes *out alloc(a) */
CU_MUST_USE int cu_file_read(CuArena *a, const char *path, CuStr *out);

/* Reads f from its current position to EOF (standard input, a pipe).
 * Leaves f open, with its position and EOF/error indicators changed.
 * Even an empty stream needs an initial buffer allocation. The buffer
 * doubles as it fills, growing in place while it is the arena's newest
 * allocation (cu_arena_grow), so a read alone in its scope holds less than
 * twice its length. */
/** regions: writes *out alloc(a) */
CU_MUST_USE int cu_stream_read(CuArena *a, FILE *f, CuStr *out);

/* Chunked, blocking input without seeks or arena allocations. A CuInput
 * owns a file opened by cu_input_open, or borrows a FILE attached by
 * cu_input_attach at its current position. Keep borrowed streams open and
 * use them exclusively until detaching. Do not copy an active owner or
 * reinitialize one before closing it. NULL arguments abort. */
typedef enum {
    CU_INPUT_ERROR = -1,
    CU_INPUT_END = 0,
    CU_INPUT_DATA = 1
} CuInputStatus;

typedef struct CuInput {
    FILE *stream;
    int owns;
    CuInputStatus state; /* next-read state, not necessarily last result */
    int error;          /* saved stdio errno; zero is possible on error */
} CuInput;

/** regions: borrows */
CU_MUST_USE int cu_input_open(CuInput *in, const char *path);
/** regions: writes *in view(stream) */
void cu_input_attach(CuInput *in, FILE *stream);
/* cap must be positive. DATA publishes 1..cap bytes in caller-owned buf;
 * END and ERROR publish *n == 0. A failed read may consume bytes and change
 * buf, but none are published. Error wins over EOF and is sticky, including
 * after failed open. A final partial DATA can cache END for the next call.
 * Existing FILE error flags are reported with errno at entry. No call
 * clears the borrowed stream's flags. Reads after successful close abort.
 * Each call goes through stdio, so it runs at read(2) speed only with
 * chunks of 64 KiB or more; smaller chunks cost measurably more per byte. */
/** regions: borrows */
CU_MUST_USE CuInputStatus cu_input_read(CuInput *in, void *buf, size_t cap, size_t *n);
/* Closes an owned stream or detaches a borrowed one. Idempotent: returns
 * the same aggregate status on repeats, 0 for any prior or close error,
 * preserving the first saved errno even when zero. No stream is retained. */
/** regions: borrows */
CU_MUST_USE int cu_input_close(CuInput *in);
/** regions: borrows */
int cu_input_errno(const CuInput *in);

/* ---- Directories ------------------------------------------------------
 *
 * One directory's entries, in no particular order, each exactly once, "."
 * and ".." skipped. A symbolic link is reported as CU_DIR_LINK and never
 * followed. The kind comes from the directory entry when the file system
 * records it, else from lstat-style status of the entry itself.
 *
 * Each name is copied into the arena given to cu_dir_open, in whatever
 * scope is innermost when cu_dir_next runs, so names outlive the listing.
 * A walker that opens a temp scope per directory, lists and sorts its
 * names, descends, then ends the scope holds memory bounded by depth times
 * directory size; a long-lived arena accumulates every name instead.
 *
 * Errors are sticky, as for CuInput: after one, cu_dir_next keeps
 * returning -1, and cu_dir_errno gives the saved errno (0 when memory ran
 * out, with the arena marked failed). Where the platform has no POSIX
 * directories, cu_dir_open fails with ENOSYS. NULL arguments abort. */
typedef enum {
    CU_DIR_FILE,
    CU_DIR_DIR,
    CU_DIR_LINK,
    CU_DIR_OTHER
} CuDirKind;

typedef struct CuDirEntry {
    CuStr name; /* NUL-terminated after len bytes, so name.ptr is a C string */
    CuDirKind kind;
} CuDirEntry;

typedef struct CuDir {
    void *handle; /* the platform's directory stream, or NULL */
    CuArena *arena;
    int state;    /* 1 listing, 0 closed, -1 failed */
    int error;    /* saved errno; zero is possible on error */
} CuDir;

/* Returns 1, or 0 with the errno saved (the CuDir is then closed and
 * cu_dir_next returns -1). */
/** regions: binds d to a */
CU_MUST_USE int cu_dir_open(CuDir *d, CuArena *a, const char *path);
/* Stores the next entry in *e and returns 1, returns 0 at the end, or -1
 * on an error, leaving *e unchanged for 0 and -1. Calls after cu_dir_close
 * abort. */
/** regions: allocates in d; writes *e view(*d) */
int cu_dir_next(CuDir *d, CuDirEntry *e);
/* Releases the stream. Idempotent; names already returned stay valid. */
/** regions: borrows */
void cu_dir_close(CuDir *d);
/** regions: borrows */
int cu_dir_errno(const CuDir *d);

/* ---- File metadata -----------------------------------------------------
 *
 * Read-only observations, not access checks, locks or stable size promises.
 * Path lookup does not pin a later open. FD lookup observes the already-open
 * object; keep that descriptor open and protected against close/reuse.
 * Neither operation allocates, opens/closes, reads, seeks or flushes.
 * See docs/METADATA.md for path-resolution and native-ABI limits. */
typedef struct CuFileInfo {
    CuDirKind kind;
    CuU64 size; /* logical bytes for CU_DIR_FILE; zero for other kinds */
} CuFileInfo;
typedef enum CuInfoResult {
    CU_INFO_ERROR = -1,
    CU_INFO_MISSING = 0,
    CU_INFO_OK = 1
} CuInfoResult;
typedef enum CuPathFollow {
    CU_PATH_NOFOLLOW,
    CU_PATH_FOLLOW
} CuPathFollow;

/* stat/lstat-style lookup. NOFOLLOW concerns the final component under
 * native path syntax, not intermediate symlinks or a symlink-free path.
 * Only ENOENT is MISSING; ENOTDIR and other failures are ERROR. Non-OK
 * leaves output unchanged and preserves the native errno, including zero.
 * Negative/unrepresentable regular-file size is ERROR/EOVERFLOW, ERANGE
 * where EOVERFLOW is unavailable. NULL pointers or unknown mode abort. */
/** regions: borrows */
CU_MUST_USE CuInfoResult cu_path_info(const char *path, CuPathFollow follow,
                                     CuFileInfo *out);
/* Borrow a native file descriptor. OK or ERROR, never MISSING; invalid
 * descriptors report EBADF on supported backends. No position/flag changes.
 * Unsupported backends return ERROR/ENOSYS, EINVAL where unavailable, after
 * pointer/mode validation and regardless of descriptor validity. */
/** regions: borrows */
CU_MUST_USE CuInfoResult cu_fd_info(int fd, CuFileInfo *out);

/* ---- File output ------------------------------------------------------
 *
 * Durable writes, for logs and content-addressed stores. A CuAppend owns
 * one open file and writes straight to it, unbuffered, always at its end.
 * Close it before reopening and do not copy an active handle. Serialize
 * operations on each handle. cu_append_write writes every byte or fails;
 * failure can leave a written prefix. A multi-call write is not an atomic
 * record against other writers. Close does not sync implicitly.
 * cu_append_sync makes writes durable through fsync, or F_FULLFSYNC where
 * the system has it. A
 * file's name is durable only once its directory has been synced too
 * (cu_dir_sync).
 *
 * Errors are sticky, except a failed lock: after one, every call but close
 * fails again with the saved errno. That matters most for sync, because
 * after a failed fsync the system may have dropped the unwritten data, and
 * a retry could falsely succeed. Where the platform has no POSIX files
 * (Cerberus), opening fails with ENOSYS. NULL arguments abort, as do calls
 * other than close after a close. */
#define CU_APPEND_CREATE 1    /* create the file if it is missing */
#define CU_APPEND_EXCLUSIVE 2 /* with CREATE: fail with EEXIST if present */
#define CU_APPEND_TRUNCATE 4  /* empty the file on open */

typedef struct CuAppend {
    int fd;    /* the descriptor, or -1 */
    int state; /* 1 open, 0 closed, -1 failed */
    int error; /* saved errno */
} CuAppend;

/* Returns 1, or 0 with the errno saved (the CuAppend is then failed, and
 * closing it is still required). New files get mode 0666 less the umask.
 * Paths must be trusted: open follows symlinks, TRUNCATE is destructive,
 * and CREATE alone does not exclude an existing file. Use CREATE with
 * EXCLUSIVE to require a new file, and private directories/umask for secrets.
 * Descriptors are close-on-exec, atomically where O_CLOEXEC is available. */
/** regions: borrows */
CU_MUST_USE int cu_append_open(CuAppend *f, const char *path, int flags);
/** regions: borrows */
CU_MUST_USE int cu_append_write(CuAppend *f, CuStr bytes);
/** regions: borrows */
CU_MUST_USE int cu_append_sync(CuAppend *f);
/* Shortens (or extends with zero bytes) the file to len bytes; writes still
 * go to its new end. Fails with EFBIG if the system's file offsets cannot
 * hold len. */
/** regions: borrows */
CU_MUST_USE int cu_append_truncate(CuAppend *f, size_t len);
/* Takes an exclusive advisory lock without waiting (flock), held until
 * close. It fails (EWOULDBLOCK) while any other open of the same file
 * holds one, in this process or another, and with ENOSYS where the
 * platform has no locks (wasm32-wasi). A failed lock sets errno but leaves
 * f usable and its saved error unchanged. Where flock is emulated with
 * POSIX record locks (some network file systems), opens within one
 * process do not exclude each other. */
/** regions: borrows */
CU_MUST_USE int cu_append_lock(CuAppend *f);
/* Closes the file. Idempotent: repeats return the same result. 0 for any
 * earlier error or a close error, keeping the first saved errno. */
/** regions: borrows */
CU_MUST_USE int cu_append_close(CuAppend *f);
/** regions: borrows */
int cu_append_errno(const CuAppend *f);

/* Path operations. Each returns 1, or 0 with errno set (ENOSYS where the
 * platform has no POSIX files). NULL arguments abort. */

/* Renames from to to, atomically replacing any file at to. */
/** regions: borrows */
CU_MUST_USE int cu_path_rename(const char *from, const char *to);
/* Removes a file (not a directory). */
/** regions: borrows */
CU_MUST_USE int cu_path_remove(const char *path);
/* Creates a directory with mode 0777 less the umask; EEXIST if anything is
 * at path. */
/** regions: borrows */
CU_MUST_USE int cu_dir_make(const char *path);
/* Makes the creations, renames and removals of names in the directory at
 * path durable. ENOSYS on wasm32-wasi, which cannot sync a directory. */
/** regions: borrows */
CU_MUST_USE int cu_dir_sync(const char *path);

/* ---- Threads -----------------------------------------------------------
 *
 * Start and join, a mutex and a condition variable, as opaque objects
 * pushed into the caller's arena: they never move, and the arena frees
 * them, so join a thread and destroy a mutex or condition before releasing
 * it. Starting and initialising can fail for real reasons and return 0
 * with errno saved: ENOSYS where the platform has no threads (wasm32-wasi,
 * Cerberus), EAGAIN when the system refuses, ENOMEM when the arena does.
 * Failure leaves the output pointer unchanged, but may consume arena
 * space. Join, lock, unlock, wait, broadcast and destroy abort on platform
 * errors or detected misuse, including joining twice or using a destroyed
 * object. Mutex ownership is not checked: do not lock recursively, unlock
 * or wait without ownership, or destroy an object still in use. There is
 * no detach: every started thread is joined. Pthreads live in libc on
 * glibc 2.34+ and musl; older glibc needs -pthread. See docs/THREADS.md. */
typedef struct CuThread CuThread;
typedef struct CuMutex CuMutex;
typedef struct CuCond CuCond;

/* Runs fn(context) on a new thread, then releases that thread's scratch
 * arenas. stack_size 0 means the platform default; one below the platform
 * minimum fails with EINVAL. The record that names the thread stays in a
 * until the arena is released. No regions contract: a value from a temp or
 * scratch scope passed as context is reported, never assumed safe. */
CU_MUST_USE int cu_thread_start(CuThread **t, CuArena *a, size_t stack_size, void (*fn)(void *),
                                void *context);
/** regions: borrows */
void cu_thread_join(CuThread *t);

/** regions: writes *m alloc(a) */
CU_MUST_USE int cu_mutex_init(CuMutex **m, CuArena *a);
/** regions: borrows */
void cu_mutex_lock(CuMutex *m);
/** regions: borrows */
void cu_mutex_unlock(CuMutex *m);
/** regions: borrows */
void cu_mutex_destroy(CuMutex *m);

/** regions: writes *c alloc(a) */
CU_MUST_USE int cu_cond_init(CuCond **c, CuArena *a);
/* Releases m, which the caller holds, and waits for a broadcast; holds m
 * again on return. Wakeups can be spurious: wait in a loop on a condition. */
/** regions: borrows */
void cu_cond_wait(CuCond *c, CuMutex *m);
/** regions: borrows */
void cu_cond_broadcast(CuCond *c);
/** regions: borrows */
void cu_cond_destroy(CuCond *c);

/* Test hook, as cu_arena_fail_at: start and init attempt n, counting from
 * 0 since this call, fails with EAGAIN and every other one succeeds.
 * (size_t)-1, the default, fails none. The count is process-wide: set it
 * only while no start or init is in progress. While enabled, make all
 * start and init attempts from one thread; disabled, they may run
 * concurrently on separate arenas. */
void cu_thread_fail_at(size_t n);

/* ---- Synchronous bounded cooperative task batches ----------------------
 * Runs an immutable job list with at most min(count, max_workers) native
 * workers. Each callback returns nonzero success or zero with errno set;
 * errno zero is still a failure. Every started worker is joined before
 * return, including after startup failure. Success means every job ran
 * exactly once. The first observed failure/cancel/deadline stops pending
 * dispatch and wins the saved error; outputs may already be partial.
 *
 * Keep jobs/contexts alive until return. Control records consume control
 * arena space even on failure; callbacks must not mutate that arena. Use
 * separate arenas/data or synchronize user state. No callback survives
 * return. There is no regions borrowing summary for run: the checker does
 * not prove its synchronous cross-thread lifetime boundary.
 *
 * One positive monotonic millisecond budget spans startup and all jobs.
 * Cancellation is cooperative: ignoring it or blocking forever can delay
 * run forever. See docs/TASKS.md for ownership, limits and a complete example. */
typedef struct CuTask CuTask;
typedef struct CuTaskSpec {
    int (*fn)(CuTask *task, void *context);
    void *context;
} CuTaskSpec;

/* control must be initialized, max_workers/timeout_ms positive and every
 * nonempty spec must have fn; invalid arguments abort. NULL jobs is valid
 * only for count=0, which succeeds without allocation/OS calls. Unsupported
 * backends report ENOSYS for nonempty batches. Returns 1, or 0 with first
 * saved errno, including zero. Release control only after run returns. */
CU_MUST_USE int cu_tasks_run(CuArena *control, const CuTaskSpec *jobs, size_t count,
                             size_t max_workers, unsigned long timeout_ms);
/* Accessors are valid only during a callback; never retain a CuTask pointer.
 * Cancelled covers explicit cancel, any failure and deadline expiry. */
/** regions: borrows */
CU_MUST_USE int cu_task_cancelled(CuTask *task);
/* Returns 1 with positive *remaining_ms rounded up, or 0 with *remaining_ms=0
 * and the first saved errno when stopped. NULL output aborts. */
/** regions: borrows */
CU_MUST_USE int cu_task_remaining(CuTask *task, unsigned long *remaining_ms);
/* Stops dispatch with ECANCELED unless already stopped or expired. */
/** regions: borrows */
void cu_task_cancel(CuTask *task);

/* ---- Subprocesses ------------------------------------------------------
 *
 * An arena-owned handle owns one direct child until wait reports DONE.
 * Wait before releasing its arena. Serialize handle operations; never reap
 * these children elsewhere or change SIGCHLD reaping policy during start
 * or while any handle is live. SIG_IGN/SA_NOCLDWAIT are incompatible.
 * Not a sandbox, shell, process-tree manager or deadline-bounded spawn.
 * See docs/PROCESSES.md for inheritance, signal and cleanup rules. */
typedef struct CuProcess CuProcess;
typedef enum CuProcessExitKind {
    CU_PROCESS_EXITED,
    CU_PROCESS_SIGNALED
} CuProcessExitKind;
typedef struct CuProcessExit {
    CuProcessExitKind kind;
    int code; /* exit status or native signal number, according to kind */
} CuProcessExit;
typedef enum CuProcessWait {
    CU_PROCESS_ERROR = -1,
    CU_PROCESS_RUNNING = 0,
    CU_PROCESS_DONE = 1
} CuProcessWait;
typedef enum CuProcessSignal {
    CU_PROCESS_TERMINATE,
    CU_PROCESS_KILL
} CuProcessSignal;

/* No shell or PATH search. argv[0] is required; vectors/strings are valid
 * NULL/NUL-terminated and borrowed only during start. NULL envp means empty
 * environment. Inherit cwd, stdio and non-CLOEXEC descriptors. Child signal
 * mask is empty and catchable dispositions default; parent state unchanged.
 * Returns 1 with ownership or 0/errno, output unchanged. Failures may consume
 * arena space. Initial auto-reaping policy is EINVAL; no backend is ENOSYS.
 * Spawn success may still lead to child exit 127 on setup/exec failure. */
/* No regions summary: posix_spawn's borrowing is not modeled. */
CU_MUST_USE int cu_process_start(CuProcess **p, CuArena *a, const char *path,
                                char *const argv[], char *const envp[]);
/* Explicit child stdio sources are borrowed native descriptors. Initialize
 * every field; INHERIT preserves the corresponding plain-start stream. */
#define CU_PROCESS_INHERIT (-1)
typedef struct CuProcessStdio {
    int input;
    int output;
    int error;
} CuProcessStdio;
/* As start, but snapshot explicit sources before child-only dup2 actions.
 * This function leaves parent descriptors, FD flags and stdio unchanged.
 * Open-file offsets/status flags still reflect child operations. Keep sources
 * stable during start, then
 * close them when desired. Explicit sources work even with FD_CLOEXEC
 * or 0/1/2 swaps. Other non-CLOEXEC descriptors still inherit, not a sandbox.
 * Invalid descriptors are EBADF; builds without atomic CLOEXEC duplication
 * report ENOSYS. Other native errors propagate. NULL stdio aborts. No pipe creation, draining or capture limits.
 * No regions summary: spawn borrowing is not modeled. */
CU_MUST_USE int cu_process_start_redirected(CuProcess **p, CuArena *a, const char *path,
                                           char *const argv[], char *const envp[],
                                           const CuProcessStdio *stdio);
/* 0 ms polls; positive ms is one monotonic waiting budget, not a child
 * lifetime limit or hard scheduling bound. EINTR does not reset it.
 * RUNNING/ERROR leave output unchanged and child owned. DONE reaps and
 * caches the result for repeat waits. Unexpected ECHILD aborts: ownership
 * was violated and signaling a possibly recycled PID would be unsafe. */
/** regions: borrows */
CU_MUST_USE CuProcessWait cu_process_wait(CuProcess *p, unsigned long timeout_ms,
                                        CuProcessExit *out);
/* SIGTERM/SIGKILL to the direct child only. Returns 1 on submission or
 * 0/errno; completion still requires wait. After DONE, 0/ESRCH, no signal.
 * NULL required pointers or unknown signal values abort. */
/** regions: borrows */
CU_MUST_USE int cu_process_signal(CuProcess *p, CuProcessSignal signal);

/* One-shot subprocess pipe communication. Linux atomic-CLOEXEC backend;
 * other platforms return ENOSYS. The bundle and child are arena-owned.
 * Close all IO ends AND wait child DONE before releasing their arena.
 * Serialize operations; no asynchronous cancellation/longjmp through cleanup. */
typedef struct CuProcessIO CuProcessIO;
typedef struct CuProcessCapture {
    char *out;
    size_t out_cap;
    size_t out_len;
    char *err;
    size_t err_cap;
    size_t err_len;
    size_t input_sent;
} CuProcessCapture;
/* Return 0/errno without child/output change, or 1 with child ownership.
 * Success does not certify exec or IO readiness: any post-spawn cleanup
 * failure is retained for pump/close. Existing start inheritance rules apply. */
CU_MUST_USE int cu_process_io_start(CuProcessIO **io, CuArena *a, const char *path,
                                   char *const argv[], char *const envp[]);
/* Borrow the same child handle, also after IO close/failure; signal/wait
 * remain caller policy. Getter/close require a valid successful-start bundle. */
CuProcess *cu_process_io_child(CuProcessIO *io);
/* Set only capture pointers/capacities; counts reset and report binary
 * prefixes/progress on every return. Nonzero lengths require valid pointers;
 * input/output/control storage must be disjoint. No NUL termination.
 * One-shot: feeds finite input and drains both outputs fairly. All parent
 * pipe ends close on success/error. No kill/reap. 1 means input accepted and
 * both outputs EOF, not child exit. EFBIG means excess output; exact cap is
 * accepted. Positive ms is one pump-entry budget; 0 disables that deadline.
 * ETIMEDOUT/other failures retain first errno, even 0. Child remains owned.
 * Writes suppress their SIGPIPE per-thread without disposition changes;
 * old pending SIGPIPE survives. Concurrent external SIGPIPE may coalesce.
 * Repeated pump/pump after close and NULL required pointers abort. */
CU_MUST_USE int cu_process_io_pump(CuProcessIO *io, CuStr input,
                                  CuProcessCapture *capture, unsigned long timeout_ms);
/* Abandon IO only. Idempotent, replays the aggregate first IO error, even0.
 * A valid-mask restoration failure during a guarded write aborts. */
CU_MUST_USE int cu_process_io_close(CuProcessIO *io);

/* ---- Sockets -----------------------------------------------------------
 *
 * Plaintext TCP over IPv4 and IPv6, without peer authentication or TLS.
 * A CuSocket owns one descriptor. Close before reopening or accepting into
 * it; do not copy an active socket. Serialize
 * all operations on each socket (including timeout changes and close).
 * Hostname resolution and cu_socket_connect remain unbounded. Numeric
 * cu_socket_connect_ip_for has its own opening budget, not a later I/O
 * timeout. Reads and writes block up to the socket's time limit, with no
 * limit by default. cu_socket_write
 * writes every byte or fails; writing to a closed peer fails with EPIPE or
 * ECONNRESET and never raises SIGPIPE. Accept never blocks.
 *
 * Errors are sticky, as with CuAppend, except on a listening socket: a
 * failed accept sets errno and leaves the listener usable, since most
 * accept failures (no connection waiting, a peer that gave up, running out
 * of descriptors) pass. A time limit running out fails with ETIMEDOUT.
 * Host names resolve with getaddrinfo; one that does not resolve fails with
 * EADDRNOTAVAIL. Where the platform has no sockets (wasm32-wasi,
 * Cerberus), opening fails with ENOSYS. NULL arguments abort, as do calls
 * other than close after a close, reading or writing a listener, accepting
 * on a connection, and closing a socket a poller watches. */
typedef struct CuSocket {
    int fd;     /* the descriptor, or -1 */
    int state;  /* 1 open, 0 closed, -1 failed, -2 closed after failure */
    int error;  /* saved errno */
    int listen; /* 1 for a listening socket */
    int watched; /* 1 while a poller watches it */
    unsigned long timeout_ms; /* per-public-call I/O budget; 0: unlimited */
} CuSocket;

/* Listens on host (a name, or an address such as "127.0.0.1" or "::1")
 * and port; port 0 picks a free one (see cu_socket_port). Returns 1, or 0
 * with the errno saved (close s even then). */
/** regions: borrows */
CU_MUST_USE int cu_socket_listen(CuSocket *s, const char *host, unsigned port, int backlog);
/* Accepts one waiting connection into out, a blocking socket. Returns 0
 * with errno set when the accept fails, EAGAIN (or EWOULDBLOCK) when no
 * connection is waiting; listener stays usable and out is not opened, so
 * do not close it. (Listen and connect are the other way round: close
 * their socket even after they fail.) out == listener aborts. */
/** regions: borrows */
CU_MUST_USE int cu_socket_accept(CuSocket *listener, CuSocket *out);
/* Connects to host and port, trying each address host resolves to. It
 * waits as long as the system does: the time limit can only be set once
 * connected. */
/** regions: borrows */
CU_MUST_USE int cu_socket_connect(CuSocket *s, const char *host, unsigned port);
/* Connect one numeric IPv4/IPv6 literal, never resolve names. Hostnames,
 * brackets, zone IDs and invalid/overlong literals fail EINVAL. Positive ms
 * is one opening budget through setup, connect and flag restoration; 0 is
 * unlimited. Connect EINTR retries a fresh socket under that same budget.
 * Returns a blocking CLOEXEC connection with read/write timeout still0,
 * or sticky failure/errno (including0). Close new FDs once, never retry a
 * possibly-consumed descriptor after EINTR; close the failed socket too.
 * Linux creation is atomic CLOEXEC/nonblocking; other POSIX backends keep
 * the existing documented fcntl inheritance window. No hard scheduling,
 * syscall or cleanup bound; unsupported backend ENOSYS. NULL s/ip abort.
 * ip is NUL-terminated and borrowed only during this call. */
/* No regions summary: inet_pton's borrowing is not modeled. */
CU_MUST_USE int cu_socket_connect_ip_for(CuSocket *s, const char *ip, unsigned port,
                                        unsigned long timeout_ms);
/* The local port s is bound to, or 0 on failure. */
/** regions: borrows */
unsigned cu_socket_port(const CuSocket *s);
/* Every later public read/write has one monotonic ms-millisecond budget,
 * including interruptions and partial writes; 0 waits forever. Timeout
 * fails with ETIMEDOUT. Accept does not wait; this setting does not bound
 * hostname DNS/connect or change an explicit numeric connect budget. */
/** regions: borrows */
CU_MUST_USE int cu_socket_set_timeout(CuSocket *s, unsigned long ms);
/* Reads at most cap bytes (cap >= 1) into buf, waiting for at least one.
 * Returns 1 with *got set; *got is 0 only once the peer has closed its
 * side. */
/** regions: borrows */
CU_MUST_USE int cu_socket_read(CuSocket *s, char *buf, size_t cap, size_t *got);
/* One read with its own millisecond budget, without changing s's configured
 * timeout. 0 means unlimited. Other read/error/ownership rules are unchanged. */
/** regions: borrows */
CU_MUST_USE int cu_socket_read_for(CuSocket *s, char *buf, size_t cap, size_t *got,
                                 unsigned long timeout_ms);
/** regions: borrows */
CU_MUST_USE int cu_socket_write(CuSocket *s, CuStr bytes);
/* Closes s. Idempotent, as cu_append_close. Failed sockets replay their
 * saved error in errno, including on repeated closes and when it is zero. */
/** regions: borrows */
CU_MUST_USE int cu_socket_close(CuSocket *s);
/** regions: borrows */
int cu_socket_errno(const CuSocket *s);

/* ---- Borrowed readers --------------------------------------------------
 * Blocking byte input over memory or a socket. No allocation, buffering,
 * ownership transfer, close or timeout-setting changes. Keep the owner and
 * source bytes alive, serialize access, do not copy an active reader, and
 * do not use the owner outside this view. END and ERROR are sticky and
 * publish *n == 0; DATA publishes 1..cap bytes. Invalid arguments abort.
 * Socket readers start one positive, monotonic millisecond budget when
 * bound; all subsequent reads, including caller pauses, share it. This is
 * not a hard real-time guarantee. Failure may consume unpublished bytes.
 * Memory readers have no time budget. Overlapping output uses memmove;
 * overwriting unread source bytes changes subsequent reads. File streams
 * are not adapted: blocking stdio cannot enforce this deadline contract. */
typedef struct CuReader {
    enum { CU_READER_MEMORY, CU_READER_SOCKET } kind;
    union { CuStr memory; CuSocket *socket; } source;
    CuInputStatus state;
    int error;
    unsigned long started_sec;
    unsigned long started_nsec;
    unsigned long timeout_ms;
} CuReader;

/* Retains bytes until the reader is abandoned. Empty may have a NULL ptr.
 * Rebinding abandons the old view without closing its owner. */
void cu_reader_memory(CuReader *r, CuStr bytes);
/* s must be initialized, open and not listening/watched; timeout_ms > 0.
 * Returns 1, or 0 with a sticky saved errno, including clock errors or
 * ENOSYS without socket/monotonic-clock support. Does not close s. */
CU_MUST_USE int cu_reader_socket(CuReader *r, CuSocket *s, unsigned long timeout_ms);
/** regions: borrows */
CU_MUST_USE CuInputStatus cu_reader_read(CuReader *r, void *buf, size_t cap, size_t *n);
/** regions: borrows */
int cu_reader_errno(const CuReader *r);

/* ---- HTTP request framing ----------------------------------------------
 * Strict HTTP/1.1 origin-form requests with one nonempty Host and optional
 * single decimal Content-Length. No Content-Length means an empty body.
 * Rejects duplicate Content-Length, comma lists, bare LF, obs-fold,
 * whitespace before colons, non-ASCII/control bytes in the head, and
 * Transfer-Encoding. Expect, Upgrade and CONNECT are unsupported too.
 * This slice is NOT a complete/conforming HTTP/1.1 server: chunked input,
 * URI/Host authority validation and routing are absent. Response heads have
 * a separate restricted serializer below; delivery/body policy remains external.
 * Never forward its raw headers as a validated proxy request.
 *
 * All limits are caller-supplied; no allocation or read-ahead. Head reading
 * currently requests one byte at a time to preserve the underlying cursor.
 * Body reads are chunked and stop exactly at Content-Length. Memory/socket
 * input uses the same parser. Bind the socket reader before begin so its
 * single budget covers the head, body and pauses between public calls.
 * Only a completely consumed successful request may be followed by another
 * on the same reader. After any error, discard the connection, not just the
 * malformed request. Already published body bytes cannot be rolled back.
 * The caller keeps reader/head storage alive and exclusively owned. Body
 * buffers must not overwrite head storage while using its metadata. */
typedef enum CuHttpResult {
    CU_HTTP_IO = -5,
    CU_HTTP_UNSUPPORTED = -4,
    CU_HTTP_TRUNCATED = -3,
    CU_HTTP_LIMIT = -2,
    CU_HTTP_BAD_REQUEST = -1,
    CU_HTTP_END = 0,
    CU_HTTP_OK = 1
} CuHttpResult;

typedef struct CuHttpRequest {
    CuReader *reader;
    CuStr method;
    CuStr target;
    CuStr head;
    size_t body_length;
    size_t remaining;
    CuHttpResult state;
    int error; /* underlying saved errno for CU_HTTP_IO, possibly zero */
} CuHttpRequest;

/* Reads/validates a head into caller storage. cap > 0 bounds its total
 * bytes, including CRLFCRLF; max_fields bounds field count and max_body
 * bounds Content-Length. Positive cap and non-NULL pointers are required.
 * On error the head buffer may be partially written but metadata is empty.
 * END means clean EOF before any head bytes. OK publishes borrowed views
 * into head, valid until the caller overwrites it. */
CU_MUST_USE CuHttpResult cu_http_begin(CuHttpRequest *r, CuReader *reader,
                                     char *head, size_t cap, size_t max_fields,
                                     size_t max_body);
/* cap > 0. OK publishes 1..cap body bytes; END publishes zero at the exact
 * body boundary. Errors are sticky, publish zero and never read again.
 * Early source EOF is TRUNCATED. A body is complete only at remaining==0.
 * IO preserves the reader's errno, including ETIMEDOUT and zero. */
/** regions: borrows */
CU_MUST_USE CuHttpResult cu_http_read(CuHttpRequest *r, void *buf, size_t cap, size_t *n);
/** regions: borrows */
int cu_http_errno(const CuHttpRequest *r);

/* ---- Restricted HTTP response heads -----------------------------------
 * Pure, allocation-free HTTP/1.1 head serialization. Status is 200..599;
 * canonical phrases are supplied for common codes, otherwise empty. Always
 * emits Connection: close and (except 204/304) exactly one Content-Length.
 * 204/205/304 require body_length==0; 204/304 omit Content-Length. A 304 zero
 * is a profile sentinel, not the selected representation's size.
 *
 * Header names are nonempty ASCII tokens. Values may be empty; otherwise
 * visible ASCII with internal SP/HTAB, no edge whitespace or other controls.
 * Case-insensitive Content-Length, Transfer-Encoding, Connection, Trailer,
 * Upgrade, TE, Keep-Alive and Proxy-Connection are owned/unsupported and rejected.
 * Other field-specific grammar, duplicate-field semantics, Date/status-required
 * metadata and application policy remain caller obligations.
 *
 * This writes HEAD BYTES ONLY, ending CRLFCRLF, never a body or terminator.
 * For ordinary responses the caller must send exactly body_length bytes;
 * for HEAD it is the selected GET representation length and no body is sent.
 * Never send a body for 204/205/304 or use this for a successful CONNECT.
 * Close after the response and do not process another request on that socket.
 * No network state, deadline or errno changes; not a complete HTTP server.
 *
 * Inputs are borrowed only during the call. All views/vectors must be valid,
 * stable and disjoint from output storage and written. headers may be NULL
 * only for count==0; head may be NULL only for cap==0. written is required.
 * On any failure *written==0 and head is unchanged; LIMIT includes size
 * arithmetic overflow. OK writes exactly *written<=cap bytes, no trailing NUL.
 * Compare the result explicitly, not as a boolean. Required NULL-pointer
 * misuse aborts; other storage/length validity remains the caller's duty. */
typedef struct CuHttpHeader {
    CuStr name;
    CuStr value;
} CuHttpHeader;

typedef enum CuHttpResponseResult {
    CU_HTTP_RESPONSE_INVALID = -2,
    CU_HTTP_RESPONSE_LIMIT = -1,
    CU_HTTP_RESPONSE_OK = 1
} CuHttpResponseResult;

CU_MUST_USE CuHttpResponseResult cu_http_response_head(char *head, size_t cap,
                                                      unsigned status, size_t body_length,
                                                      const CuHttpHeader *headers, size_t count,
                                                      size_t *written);

/* ---- Poller ------------------------------------------------------------
 *
 * Watches many sockets from one thread and says which became readable,
 * the way a server parks idle connections. The mechanism is private (POSIX
 * poll() today), so epoll or kqueue can replace it without changing
 * callers.
 *
 * A watch is one-shot: once wait reports a socket, the poller forgets it,
 * so another thread can use it safely; add it again to watch it again. A
 * watch can also carry an idle limit, after which wait reports it as
 * expired and forgets it. Any thread may call add and wake; one thread at
 * a time calls wait. The poller is pushed into the caller's arena: destroy
 * it before releasing the arena. A watched socket must stay open until wait
 * reports it, and must not be added again until then: both abort. Failures
 * to create report ENOSYS where there are no sockets or threads, else the
 * system's errno. */
typedef struct CuPoller CuPoller;

typedef struct CuReady {
    void *tag;   /* what add was given */
    int expired; /* 1: the idle limit ran out; 0: readable */
} CuReady;

/* Creates a poller that can watch up to capacity sockets at once. */
/** regions: writes *p alloc(a) */
CU_MUST_USE int cu_poller_init(CuPoller **p, CuArena *a, size_t capacity);
/* Watches s until it is readable, then reports tag. idle_ms 0 means no
 * idle limit. Fails with ENOSPC when capacity sockets are watched, and with
 * s's saved error if s has failed. s must stay where it is until
 * reported. No
 * regions contract: the poller keeps tag, as cu_thread_start keeps its
 * context. */
CU_MUST_USE int cu_poller_add(CuPoller *p, CuSocket *s, void *tag, unsigned long idle_ms);
/* Waits up to max_ms (0: forever) for readable or expired watches, or a
 * wake, and stores at most cap of them in out. Returns 1 with *n set (it
 * may be 0), or 0 with errno set. No regions contract: the tags it hands
 * back are the ones add was given. */
CU_MUST_USE int cu_poller_wait(CuPoller *p, unsigned long max_ms, CuReady *out, size_t cap, size_t *n);
/* Makes a wait in progress, or the next one, return. */
/** regions: borrows */
void cu_poller_wake(CuPoller *p);
/* Forgets every watch; the sockets stay open and can be closed. */
/** regions: borrows */
void cu_poller_destroy(CuPoller *p);

#endif
