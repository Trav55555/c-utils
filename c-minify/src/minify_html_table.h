#ifndef MINIFY_HTML_TABLE_H
#define MINIFY_HTML_TABLE_H
#include <stddef.h>
#include "minify_html_hash.h"

/* minify/html's generated tables (hash.go, table.go), built by
 * tools/gen_tables.py into minify_html_table.c. */

/* table.go traits. Tags and attributes have separate bit sets. */
#define MH_NORMAL_TAG 1u
#define MH_RAW_TAG 2u
#define MH_BLOCK_TAG 4u
#define MH_OBJECT_TAG 8u
#define MH_OMIT_P_TAG 16u
#define MH_KEEP_P_TAG 32u

#define MH_BOOLEAN_ATTR 1u
#define MH_URL_ATTR 2u
#define MH_TRIM_ATTR 4u

typedef struct MinifyHtmlTraits {
    unsigned long hash;
    unsigned traits;
} MinifyHtmlTraits;

/* One EntitiesMap entry: a name and the bytes it is replaced with. */
typedef struct MinifyEntity {
    const char *name;
    unsigned char name_len;
    const char *value;
    unsigned char value_len;
} MinifyEntity;

/* hash.go ToHash: the MH_* value naming s, or 0. Case sensitive. */
unsigned long minify_html_to_hash(const unsigned char *s, size_t n);
/* table.go tagMap[hash] and attrMap[hash]; 0 when absent. */
unsigned minify_html_tag_traits(unsigned long hash);
unsigned minify_html_attr_traits(unsigned long hash);
/* table.go jsMimetypes[s] */
int minify_html_js_mimetype(const unsigned char *s, size_t n);
/* table.go EntitiesMap[name], or NULL. */
const MinifyEntity *minify_html_entity(const unsigned char *name, size_t n);

#endif
