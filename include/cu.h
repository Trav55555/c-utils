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

#endif
