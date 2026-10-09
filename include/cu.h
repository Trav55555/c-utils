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
    size_t blocks;     /* blocks ever made; numbers the next one, never reset */
} CuArena;

/* A position in an arena. Ending it frees everything pushed since. The block
 * is held by number, not address: a freed block's address can come back from
 * malloc, and a freed pointer must not even be compared. */
typedef struct CuTemp {
    CuArena *arena;
    size_t block; /* number of the newest block, 0 if there was none */
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

/* ---- Binary reading and writing ------------------------------------- */

/* Fixed-width unsigned integers in little (le) or big (be) endian order,
 * read from s at *pos. On success each reader stores the value in *out,
 * advances *pos past the bytes read, and returns 1. If fewer bytes remain,
 * or *pos > s.len, it returns 0 and leaves *pos and *out unchanged. Bytes
 * are assembled arithmetically, so input alignment and host byte order do
 * not matter. */
CU_MUST_USE int cu_rd_u8(CuStr s, size_t *pos, unsigned long *out);
CU_MUST_USE int cu_rd_u16le(CuStr s, size_t *pos, unsigned long *out);
CU_MUST_USE int cu_rd_u16be(CuStr s, size_t *pos, unsigned long *out);
CU_MUST_USE int cu_rd_u32le(CuStr s, size_t *pos, unsigned long *out);
CU_MUST_USE int cu_rd_u32be(CuStr s, size_t *pos, unsigned long *out);
/* The next n bytes as a view into s, under the same rules. */
CU_MUST_USE int cu_rd_bytes(CuStr s, size_t *pos, size_t n, CuStr *out);

/* Append v in the given width and byte order. A v that does not fit the
 * width aborts. Failure is sticky, as for every builder append. */
void cu_sb_u8(CuBuilder *b, unsigned long v);
void cu_sb_u16le(CuBuilder *b, unsigned long v);
void cu_sb_u16be(CuBuilder *b, unsigned long v);
void cu_sb_u32le(CuBuilder *b, unsigned long v);
void cu_sb_u32be(CuBuilder *b, unsigned long v);

/* ---- Growable array ---------------------------------------------------- */

/* Elements of one type, stored in an arena. Use the CU_VEC_* macros: each
 * passes sizeof the element type at the call site, and a size that does not
 * match the vector's element size aborts, so most type mix-ups are caught.
 * Pushes never fail loudly: a failure marks the vector, the push is skipped,
 * and cu_vec_failed reports it. Growth moves the elements, so pointers from
 * CU_VEC_AT and CU_VEC_DATA are valid only until the next push or reserve. */
typedef struct CuVec {
    CuArena *arena;
    char *data;
    size_t len;
    size_t cap;
    size_t elem_size;
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

/* elem_size 0 aborts. */
void cu_vec_init(CuVec *v, CuArena *a, size_t elem_size);
/* Room for at least extra more elements without moving. Returns 0 and marks
 * the vector failed on overflow or out of memory. */
int cu_vec_reserve(CuVec *v, size_t extra);
size_t cu_vec_len(const CuVec *v);
int cu_vec_failed(const CuVec *v);
/* Sets len to 0 and keeps the storage. */
void cu_vec_clear(CuVec *v);

void cu_vec_push_(CuVec *v, const void *elem, size_t elem_size);
int cu_vec_pop_(CuVec *v, void *out, size_t elem_size);
void *cu_vec_at_(const CuVec *v, size_t i, size_t elem_size);
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

CuSlice cu_vec_slice(const CuVec *v);
/* Elements [start, end), with end clamped to len and start clamped to end,
 * so it never makes a view outside s. */
CuSlice cu_slice_sub(CuSlice s, size_t start, size_t end);

CuSlice cu_slice_n_(void *p, size_t n, size_t elem_size);
void *cu_slice_at_(CuSlice s, size_t i, size_t elem_size);
void *cu_slice_data_(CuSlice s, size_t elem_size);

/* ---- Hash map ---------------------------------------------------------- */

/* 32-bit FNV-1a of s's bytes. */
unsigned long cu_hash_fnv1a(CuStr s);

/* A hash trie (Wellons' design) from CuStr keys to values of one type, in
 * an arena. Each node has four children chosen by successive 2-bit slices
 * of the key's hash: nothing is rehashed, entries never move, and nothing
 * is freed individually. Keys are copied into the arena. There is no
 * removal. Value pointers stay valid until the arena is released or a temp
 * scope that was open at insertion ends.
 *
 * Cost per call is the trie's depth, logarithmic for ordinary keys. Keys
 * whose 32-bit hashes are equal share one path that ends in a list, so n
 * such keys cost O(n) each; FNV-1a collisions are easy to construct, so
 * bound the number of keys taken from untrusted input.
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
void cu_map_init(CuMap *m, CuArena *a, size_t value_size);
size_t cu_map_len(const CuMap *m);
int cu_map_failed(const CuMap *m);
CuMapIter cu_map_iter(const CuMap *m);

void *cu_map_upsert_(CuMap *m, CuStr key, const void *init, size_t init_size, size_t value_size);
void *cu_map_get_(const CuMap *m, CuStr key, size_t value_size);
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
 * released. As with CuVec, a size that does not match the pool's value size
 * aborts. A failed addition (out of memory) marks the pool and returns the
 * null handle; later additions also fail, and cu_pool_failed reports it. */
typedef struct CuPool {
    CuArena *arena;
    CuVec pages;
    size_t slots;     /* slots ever made */
    size_t free_head; /* first reusable slot, or (size_t)-1 */
    size_t len;       /* live objects */
    size_t value_size;
    unsigned long max_gen;
    int failed;
} CuPool;

#define CU_POOL_INIT(p, a, T) cu_pool_init((p), (a), sizeof(T))
/* Adds a copy of *v; returns its handle, or the null handle on failure. */
#define CU_POOL_ADD(p, v) cu_pool_add_((p), (v), sizeof *(v))
/* Pointer to h's object, or NULL if h is stale, null or forged. */
#define CU_POOL_GET(p, T, h) ((T *)cu_pool_get_((p), (h), sizeof(T)))

/* Generations up to the largest unsigned long. value_size 0 aborts. */
void cu_pool_init(CuPool *p, CuArena *a, size_t value_size);
/* Generations up to max_gen, which must be at least 1. */
void cu_pool_init_with(CuPool *p, CuArena *a, size_t value_size, unsigned long max_gen);
/* Removes h's object. Returns 0, changing nothing, if h is stale, null or
 * forged. */
int cu_pool_remove(CuPool *p, CuHandle h);
size_t cu_pool_len(const CuPool *p);
int cu_pool_failed(const CuPool *p);
CuHandle cu_handle_null(void);
int cu_handle_is_null(CuHandle h);

CuHandle cu_pool_add_(CuPool *p, const void *v, size_t value_size);
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
 * removed. A failed registration or addition (out of memory) marks the
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

void cu_plex_init(CuPlex *px, CuArena *a);
unsigned cu_plex_kind(CuPlex *px, size_t node_size);
/* Removes link's node. Returns 0, changing nothing, if link is null, stale
 * or names an unknown kind. */
int cu_plex_remove(CuPlex *px, CuLink link);
int cu_plex_failed(const CuPlex *px);
CuLink cu_link_null(void);
int cu_link_is_null(CuLink link);
/* Whether a and b name the same node; a stale link still equals itself. */
int cu_link_eq(CuLink a, CuLink b);

CuLink cu_plex_add_(CuPlex *px, unsigned kind, const void *v, size_t node_size);
void *cu_plex_get_(const CuPlex *px, unsigned kind, CuLink link, size_t node_size);

#endif
