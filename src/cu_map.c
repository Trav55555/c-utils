#include <stdlib.h>
#include <string.h>
#include "cu.h"

struct CuMapNode {
    CuMapNode *child[4];
    CuMapNode *next; /* insertion order */
    CuStr key;
    unsigned long hash; /* cu_hash_fnv1a(key), compared before key's bytes */
};

/* Its size is a multiple of every member's alignment. */
typedef union {
    long l;
    double d;
    long double ld;
    void *p;
    void (*f)(void);
} MaxAlign;

/* The largest power of two dividing MaxAlign's size: every alignment
 * divides it, as in cu_arena.c. */
#define MAP_ALIGN (sizeof(MaxAlign) & (0 - sizeof(MaxAlign)))

/* An entry is one arena push, aligned for any type: the node, then
 * value_size bytes of value at the node's size rounded up to MAP_ALIGN, so
 * aligned for any type too, then the key's bytes. */
#define VALUE_OFFSET ((sizeof(CuMapNode) + MAP_ALIGN - 1) & ~(MAP_ALIGN - 1))

#define CU_HASH_MASK 0xFFFFFFFFUL

unsigned long cu_hash_fnv1a(CuStr s)
{
    unsigned long h;
    size_t i;

    h = 0x811c9dc5UL;
    for (i = 0; i < s.len; i++) {
        h ^= (unsigned long)(unsigned char)s.ptr[i];
        h = (h * 0x01000193UL) & CU_HASH_MASK;
    }
    return h;
}

void cu_map_init(CuMap *m, CuArena *a, size_t value_size)
{
    if (m == NULL || a == NULL || value_size == 0)
        abort();
    m->arena = a;
    m->root = NULL;
    m->first = NULL;
    m->last = NULL;
    m->len = 0;
    m->value_size = value_size;
    m->scope = cu_arena_scope(a);
    m->failed = 0;
}

size_t cu_map_len(const CuMap *m)
{
    if (m == NULL)
        abort();
    return m->len;
}

int cu_map_failed(const CuMap *m)
{
    if (m == NULL)
        abort();
    return m->failed;
}

static char *value_of(CuMapNode *n)
{
    return (char *)n + VALUE_OFFSET;
}

/* Whether n holds key, whose hash is h. Most nodes on a path differ in
 * hash, so their key bytes are never read. */
static int holds(const CuMapNode *n, CuStr key, unsigned long h)
{
    return n->hash == h && n->key.len == key.len && memcmp(n->key.ptr, key.ptr, key.len) == 0;
}

/* The link that holds key's node, or the empty link where it belongs. The
 * top two bits of the hash pick each child; after 16 levels the hash is
 * spent and every key left on the path goes to child 0, a list. */
static CuMapNode **find(CuMapNode **link, CuStr key, unsigned long h)
{
    unsigned long bits;

    bits = h;
    while (*link != NULL && !holds(*link, key, h)) {
        link = &(*link)->child[bits >> 30];
        bits = (bits << 2) & CU_HASH_MASK;
    }
    return link;
}

static CuMapNode *insert(CuMap *m, CuMapNode **link, CuStr key, unsigned long h, const void *init)
{
    CuMapNode *n;
    char *bytes;
    size_t size;

    if (!cu_size_add(VALUE_OFFSET, m->value_size, &size) || !cu_size_add(size, key.len, &size)) {
        m->arena->failed = 1;
        return NULL;
    }
    n = (CuMapNode *)cu_arena_push(m->arena, 1, size);
    if (n == NULL)
        return NULL;
    bytes = value_of(n) + m->value_size;
    memcpy(value_of(n), init, m->value_size);
    if (key.len != 0)
        memcpy(bytes, key.ptr, key.len);
    n->child[0] = NULL;
    n->child[1] = NULL;
    n->child[2] = NULL;
    n->child[3] = NULL;
    n->next = NULL;
    n->key = cu_str_n(bytes, key.len);
    n->hash = h;
    if (m->last != NULL)
        m->last->next = n;
    else
        m->first = n;
    m->last = n;
    m->len++;
    *link = n;
    return n;
}

void *cu_map_upsert_(CuMap *m, CuStr key, const void *init, size_t init_size, size_t value_size)
{
    CuMapNode **link;
    CuMapNode *n;
    unsigned long h;

    if (m == NULL || init == NULL || init_size != m->value_size || value_size != m->value_size)
        abort();
    /* Checked even for a key already present, so a wrong scope aborts
     * whatever the keys. */
    if (m->arena->scope != m->scope)
        abort();
    h = cu_hash_fnv1a(key);
    link = find(&m->root, key, h);
    if (*link != NULL)
        return value_of(*link);
    if (m->failed)
        return NULL;
    n = insert(m, link, key, h, init);
    if (n == NULL) {
        m->failed = 1;
        return NULL;
    }
    return value_of(n);
}

/* Walks nodes rather than links, so nothing local has its address taken. */
void *cu_map_get_(const CuMap *m, CuStr key, size_t value_size)
{
    CuMapNode *n;
    unsigned long h;
    unsigned long bits;

    if (m == NULL || value_size != m->value_size)
        abort();
    h = cu_hash_fnv1a(key);
    bits = h;
    n = m->root;
    while (n != NULL && !holds(n, key, h)) {
        n = n->child[bits >> 30];
        bits = (bits << 2) & CU_HASH_MASK;
    }
    return n != NULL ? value_of(n) : NULL;
}

CuMapIter cu_map_iter(const CuMap *m)
{
    CuMapIter it;

    if (m == NULL)
        abort();
    it.node = m->first;
    it.value_size = m->value_size;
    return it;
}

void *cu_map_next_(CuMapIter *it, CuStr *key, size_t value_size)
{
    CuMapNode *n;

    if (it == NULL || key == NULL || value_size != it->value_size)
        abort();
    n = it->node;
    if (n == NULL)
        return NULL;
    it->node = n->next;
    *key = n->key;
    return value_of(n);
}
