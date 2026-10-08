#include <stdlib.h>
#include <string.h>
#include "cu.h"

struct CuMapNode {
    CuMapNode *child[4];
    CuMapNode *next; /* insertion order */
    CuStr key;
    void *value;
};

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

/* The link that holds key's node, or the empty link where it belongs. The
 * top two bits of the hash pick each child; after 16 levels the hash is
 * spent and every key left on the path goes to child 0, a list. */
static CuMapNode **find(CuMapNode **link, CuStr key)
{
    unsigned long h;

    h = cu_hash_fnv1a(key);
    while (*link != NULL && !cu_str_eq((*link)->key, key)) {
        link = &(*link)->child[h >> 30];
        h = (h << 2) & CU_HASH_MASK;
    }
    return link;
}

static CuMapNode *insert(CuMap *m, CuMapNode **link, CuStr key, const void *init)
{
    CuMapNode *n;
    char *bytes;
    void *value;

    n = CU_PUSH(m->arena, CuMapNode, 1);
    value = cu_arena_push(m->arena, 1, m->value_size);
    bytes = key.len != 0 ? cu_arena_push_bytes(m->arena, key.len) : NULL;
    if (n == NULL || value == NULL || (key.len != 0 && bytes == NULL))
        return NULL;
    if (key.len != 0)
        memcpy(bytes, key.ptr, key.len);
    memcpy(value, init, m->value_size);
    n->child[0] = NULL;
    n->child[1] = NULL;
    n->child[2] = NULL;
    n->child[3] = NULL;
    n->next = NULL;
    n->key = cu_str_n(bytes, key.len);
    n->value = value;
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

    if (m == NULL || init == NULL || init_size != m->value_size || value_size != m->value_size)
        abort();
    link = find(&m->root, key);
    if (*link != NULL)
        return (*link)->value;
    if (m->failed)
        return NULL;
    if (insert(m, link, key, init) == NULL) {
        m->failed = 1;
        return NULL;
    }
    return (*link)->value;
}

void *cu_map_get_(const CuMap *m, CuStr key, size_t value_size)
{
    CuMapNode *root;
    CuMapNode **link;

    if (m == NULL || value_size != m->value_size)
        abort();
    root = m->root;
    link = find(&root, key);
    return *link != NULL ? (*link)->value : NULL;
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
    return n->value;
}
