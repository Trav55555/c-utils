#include "cu.h"
#include <stdlib.h>
#include <string.h>

/* C89 has no thread-local storage. Where the compiler offers none, the
 * scratch arenas are process-wide and must be used from one thread only. */
#if defined(_MSC_VER)
#define CU_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__TINYC__)
#define CU_THREAD_LOCAL __thread
#else
#define CU_THREAD_LOCAL
#endif

#if defined(CU_ARENA_DEBUG) || defined(__SANITIZE_ADDRESS__)
#define CU_DEBUG_DEFAULT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CU_DEBUG_DEFAULT 1
#endif
#endif
#ifndef CU_DEBUG_DEFAULT
#define CU_DEBUG_DEFAULT 0
#endif

#define CU_DEFAULT_BLOCK_SIZE 65536
#define CU_SCRATCH_COUNT 2

/* Its size is a multiple of every member's alignment. */
typedef union {
  long l;
  double d;
  long double ld;
  void *p;
  void (*f)(void);
} CuMaxAlign;

/* The largest power of two dividing that size. Alignments are powers of
 * two, so each member's divides it, and offsets that are multiples of it
 * from a malloc'd base are aligned for any type. It is a power of two even
 * where the size is not (12 for i386's long double), so push_aligned can
 * mask instead of divide. */
#define CU_ALIGN (sizeof(CuMaxAlign) & (0 - sizeof(CuMaxAlign)))

/* Data starts header_size() bytes after the block. */
struct CuBlock {
  CuBlock *prev;
  size_t cap;
  size_t used;
  size_t num; /* 1 for an arena's first block, then counting up */
};

static CU_THREAD_LOCAL CuArena scratch[CU_SCRATCH_COUNT];
static CU_THREAD_LOCAL int scratch_ready;

int cu_size_add(size_t a, size_t b, size_t *out) {
  if (out == NULL)
    abort();
  if (a > (size_t)-1 - b)
    return 0;
  *out = a + b;
  return 1;
}

int cu_size_mul(size_t a, size_t b, size_t *out) {
  if (out == NULL)
    abort();
  if (b != 0 && a > (size_t)-1 / b)
    return 0;
  *out = a * b;
  return 1;
}

static size_t header_size(void) {
  return (sizeof(CuBlock) + CU_ALIGN - 1) / CU_ALIGN * CU_ALIGN;
}

static char *block_data(CuBlock *b) { return (char *)b + header_size(); }

/* A fresh scope id. Unsigned, so it wraps rather than overflows; ids repeat
 * only after (size_t)-1 of them on one arena. 0 is never an id, so a zeroed
 * CuTemp or container never matches. */
static size_t next_scope_id(CuArena *a) {
  a->scopes++;
  if (a->scopes == 0)
    a->scopes = 1;
  return a->scopes;
}

void cu_arena_init_with(CuArena *a, size_t block_size, int debug) {
  if (a == NULL)
    abort();
  a->head = NULL;
  a->block_size = block_size;
  a->debug = debug;
  a->failed = 0;
  a->blocks = 0;
  a->held = 0;
  a->limit = (size_t)-1;
  a->refused = 0;
  a->attempts = 0;
  a->fail_at = (size_t)-1;
  a->scopes = 0;
  a->scope = next_scope_id(a);
  a->mark_block = 0;
  a->mark_used = 0;
  a->cache = NULL;
  a->cached = 0;
}

void cu_arena_init(CuArena *a) {
  cu_arena_init_with(a, CU_DEFAULT_BLOCK_SIZE, CU_DEBUG_DEFAULT);
}

/* Whether held can grow by n bytes under the limit. */
static int fits_limit(const CuArena *a, size_t n) {
  return a->held <= a->limit && n <= a->limit - a->held;
}

static void drop_cache(CuArena *a) {
  free(a->cache);
  a->cache = NULL;
  a->cached = 0;
}

/* Frees the blocks newer than stop. With keep set, the largest of them is
 * cached instead, if it is larger than the one already cached and held +
 * its size fits the limit (which may have been lowered since). */
static void free_blocks_until(CuArena *a, CuBlock *stop, int keep) {
  CuBlock *b;
  CuBlock *prev;

  b = a->head;
  while (b != stop) {
    prev = b->prev;
    a->held -= header_size() + b->cap;
    if (keep && (a->cache == NULL || b->cap > a->cache->cap) &&
        fits_limit(a, header_size() + b->cap)) {
      drop_cache(a);
      a->cache = b;
      a->cached = header_size() + b->cap;
    } else {
      free(b);
    }
    b = prev;
  }
  a->head = stop;
}

/* The cached block, now held, if it has room for cap bytes and the limit
 * allows it; else NULL, changing nothing. */
static CuBlock *take_cache(CuArena *a, size_t cap) {
  CuBlock *b;

  b = a->cache;
  if (b == NULL || b->cap < cap || !fits_limit(a, a->cached))
    return NULL;
  a->held += a->cached;
  a->cache = NULL;
  a->cached = 0;
  return b;
}

void cu_arena_release(CuArena *a) {
  if (a == NULL)
    abort();
  free_blocks_until(a, NULL, 0);
  drop_cache(a);
  a->failed = 0;
  a->refused = 0;
  /* A new base scope: open temp scopes, and containers made before now,
   * no longer match, so ending or growing one aborts. */
  a->scope = next_scope_id(a);
  a->mark_block = 0;
  a->mark_used = 0;
}

/* align must be a power of two: 1 or CU_ALIGN. */
static void *push_aligned(CuArena *a, size_t size, size_t align) {
  CuBlock *b;
  size_t pad;
  size_t room;
  size_t cap;
  size_t total;
  size_t num;
  char *p;

  if (a == NULL)
    abort();
  b = a->head;
  if (b != NULL && !a->debug) {
    pad = (0 - b->used) & (align - 1);
    room = b->cap - b->used;
    if (pad <= room && size <= room - pad) {
      p = block_data(b) + b->used + pad;
      b->used += pad + size;
      return p;
    }
  }
  cap = size;
  if (!a->debug && cap < a->block_size)
    cap = a->block_size;
  if (!cu_size_add(header_size(), cap, &total) ||
      !cu_size_add(a->blocks, 1, &num)) {
    a->failed = 1;
    return NULL;
  }
  /* Counting wraps only after 2^N attempts; the injected one fails. */
  if (a->attempts++ == a->fail_at) {
    a->failed = 1;
    return NULL;
  }
  b = take_cache(a, cap);
  if (b == NULL) {
    /* The cached block is too small or past the limit. It goes before
     * malloc, so held + cached stays under the limit and one unusable
     * block is not kept beside a new one. */
    drop_cache(a);
    if (!fits_limit(a, total)) {
      a->refused = total;
      a->failed = 1;
      return NULL;
    }
    b = (CuBlock *)malloc(total);
    if (b == NULL) {
      a->failed = 1;
      return NULL;
    }
    a->held += total;
    b->cap = cap;
  }
  b->prev = a->head;
  b->used = size;
  b->num = num;
  a->blocks = num;
  a->head = b;
  return block_data(b);
}

void *cu_arena_push(CuArena *a, size_t count, size_t elem_size) {
  size_t size;

  if (a == NULL)
    abort();
  if (!cu_size_mul(count, elem_size, &size)) {
    a->failed = 1;
    return NULL;
  }
  return push_aligned(a, size, CU_ALIGN);
}

char *cu_arena_push_bytes(CuArena *a, size_t n) {
  return (char *)push_aligned(a, n, 1);
}

/* Whether the bytes from off in the head block b were pushed under the
 * innermost scope. The head is the newest block, so its number is at least
 * the mark's; a mark of block 0 began before every block. */
static int pushed_in_scope(const CuArena *a, const CuBlock *b, size_t off) {
  return b->num != a->mark_block || off >= a->mark_used;
}

/* Reallocates the head block, whose one allocation starts at its data, to
 * hold size > its cap bytes. Accounted as a block of the bytes added. */
static void *grow_head(CuArena *a, size_t size) {
  CuBlock *b;
  size_t total;
  size_t extra;

  if (!cu_size_add(header_size(), size, &total)) {
    a->failed = 1;
    return NULL;
  }
  extra = size - a->head->cap;
  if (a->attempts++ == a->fail_at) {
    a->failed = 1;
    return NULL;
  }
  if (!fits_limit(a, extra)) {
    a->refused = extra;
    a->failed = 1;
    return NULL;
  }
  /* Keep held + cached under the limit. */
  if (a->cached > a->limit - a->held - extra)
    drop_cache(a);
  /* On failure the block is untouched and still the head. Nothing but
   * a->head points at it: block links run from newer to older. */
  b = (CuBlock *)realloc(a->head, total);
  if (b == NULL) {
    a->failed = 1;
    return NULL;
  }
  a->held += extra;
  b->cap = size;
  b->used = size;
  a->head = b;
  return block_data(b);
}

/* The head block, if p (old_size > 0 bytes) is its newest allocation and
 * was pushed under the innermost scope; else NULL. p is the newest
 * allocation when it ends where the head block's used bytes do. Only
 * equality is compared, which is defined for any two pointers, and the
 * candidate address is formed inside the block. */
static CuBlock *newest_in_scope(const CuArena *a, const void *p, size_t old_size) {
  CuBlock *b;

  b = a->head;
  if (b != NULL && old_size <= b->used &&
      (const char *)p == block_data(b) + (b->used - old_size) &&
      pushed_in_scope(a, b, b->used - old_size))
    return b;
  return NULL;
}

int cu_arena_extend(CuArena *a, void *p, size_t old_size, size_t new_size) {
  CuBlock *b;

  if (a == NULL || p == NULL)
    abort();
  if (new_size <= old_size)
    return 1;
  if (old_size == 0)
    return 0;
  b = newest_in_scope(a, p, old_size);
  if (b == NULL || new_size - old_size > b->cap - b->used)
    return 0;
  b->used += new_size - old_size;
  return 1;
}

void *cu_arena_grow(CuArena *a, void *p, size_t old_size, size_t new_size) {
  CuBlock *b;
  char *q;

  if (a == NULL || (p == NULL && old_size != 0))
    abort();
  /* Nothing to keep. A zero-size allocation can sit exactly where a later
   * scope began, so growing it in place could take that scope's memory. */
  if (old_size == 0)
    return push_aligned(a, new_size, CU_ALIGN);
  if (cu_arena_extend(a, p, old_size, new_size))
    return p;
  /* Reallocate only a block made inside the innermost scope: the scope's
   * end keeps the block it began in, so a bigger one would outlive it. */
  b = newest_in_scope(a, p, old_size);
  if (b != NULL && b->used == old_size && b->num != a->mark_block)
    return grow_head(a, new_size);
  q = (char *)cu_arena_push(a, new_size, 1);
  if (q != NULL)
    memcpy(q, p, old_size);
  return q;
}

void cu_arena_set_limit(CuArena *a, size_t max_bytes) {
  if (a == NULL)
    abort();
  a->limit = max_bytes;
  if (!fits_limit(a, a->cached))
    drop_cache(a);
}

void cu_arena_fail_at(CuArena *a, size_t n) {
  if (a == NULL)
    abort();
  a->fail_at = n;
}

int cu_arena_failed(const CuArena *a) {
  if (a == NULL)
    abort();
  return a->failed;
}

CuTemp cu_temp_begin(CuArena *a) {
  CuTemp t;

  if (a == NULL)
    abort();
  t.arena = a;
  t.block = a->head != NULL ? a->head->num : 0;
  t.used = a->head != NULL ? a->head->used : 0;
  t.id = next_scope_id(a);
  t.parent = a->scope;
  t.parent_block = a->mark_block;
  t.parent_used = a->mark_used;
  a->scope = t.id;
  a->mark_block = t.block;
  a->mark_used = t.used;
  return t;
}

void cu_temp_end(CuTemp t) {
  CuBlock *b;

  if (t.arena == NULL)
    abort();
  /* Only the innermost scope may end. This catches scopes ended out of
   * order, ended twice, or ended after the arena was released. */
  if (t.id != t.arena->scope)
    abort();
  /* Backstops for a forged CuTemp: its block must still be in the chain
   * and its position not past the current one. Block numbers are never
   * reused, so a freed block cannot match. */
  b = t.arena->head;
  while (b != NULL && b->num != t.block)
    b = b->prev;
  if (b == NULL && t.block != 0)
    abort();
  free_blocks_until(t.arena, b, !t.arena->debug);
  if (b != NULL) {
    if (t.used > b->used)
      abort();
    b->used = t.used;
  }
  t.arena->scope = t.parent;
  t.arena->mark_block = t.parent_block;
  t.arena->mark_used = t.parent_used;
}

size_t cu_arena_scope(const CuArena *a) {
  if (a == NULL)
    abort();
  return a->scope;
}

CuTemp cu_scratch_begin(CuArena **conflicts, size_t count) {
  size_t i;
  size_t j;
  int clash;

  if (conflicts == NULL && count != 0)
    abort();
  if (!scratch_ready) {
    for (i = 0; i < CU_SCRATCH_COUNT; i++)
      cu_arena_init(&scratch[i]);
    scratch_ready = 1;
  }
  for (i = 0; i < CU_SCRATCH_COUNT; i++) {
    clash = 0;
    for (j = 0; j < count; j++) {
      if (conflicts[j] == &scratch[i])
        clash = 1;
    }
    if (!clash)
      return cu_temp_begin(&scratch[i]);
  }
  abort();
  return cu_temp_begin(&scratch[0]);
}

CuTemp cu_scratch_begin1(CuArena *conflict) {
  CuArena *conflicts[1];

  conflicts[0] = conflict;
  return cu_scratch_begin(conflicts, 1);
}

void cu_scratch_end(CuTemp t) { cu_temp_end(t); }

void cu_scratch_release(void) {
  size_t i;

  if (!scratch_ready)
    return;
  /* Released, not reinitialised: a reinitialised arena would number its
   * scopes from 1 again, and a stale scope could match a new one. */
  for (i = 0; i < CU_SCRATCH_COUNT; i++)
    cu_arena_release(&scratch[i]);
}
