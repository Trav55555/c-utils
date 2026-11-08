/* goldmark ast/ast.go, ast/block.go, ast/inline.go and the node
 * constructors of extension/ast as C. Each function names the Go function
 * it ports. */
#include "mark_ast.h"
#include "mark_util.h"

MarkNode *mark_node_new(CuArena *a, MarkKind kind)
{
    MarkNode *n = CU_PUSH(a, MarkNode, 1);

    if (n == NULL) return NULL;
    n->kind = kind;
    n->parent = NULL;
    n->first = NULL;
    n->last = NULL;
    n->prev = NULL;
    n->next = NULL;
    n->child_count = 0;
    CU_VEC_INIT(&n->attrs, a, MarkAttr);
    n->blank_previous_lines = 0;
    n->lines = NULL;
    n->level = 0;
    n->marker = 0;
    n->is_tight = kind == MARK_LIST; /* ast.NewList starts tight */
    n->start = 0;
    n->offset = 0;
    n->html_block_type = 0;
    n->closure_line = mark_segment(-1, -1); /* ast.NewHTMLBlock */
    n->info = NULL;
    n->has_language = 0;
    n->language = cu_str_empty();
    n->segment = mark_segment(0, 0);
    n->flags = 0;
    n->value = cu_str_empty();
    n->destination = cu_str_empty();
    n->title = cu_str_empty();
    n->has_title = 0;
    n->autolink_type = 0;
    n->autolink_value = NULL;
    n->protocol = cu_str_empty();
    n->segments = NULL;
    n->can_open = 0;
    n->can_close = 0;
    n->length = 0;
    n->original_length = 0;
    n->delim_char = 0;
    n->prev_delim = NULL;
    n->next_delim = NULL;
    n->is_image = 0;
    n->ll_prev = NULL;
    n->ll_next = NULL;
    n->ll_first = NULL;
    n->ll_last = NULL;
    n->alignments = NULL;
    n->alignment = kind == MARK_TABLE_CELL ? MARK_ALIGN_NONE : 0; /* ast.NewTableCell */
    n->is_checked = 0;
    n->temporary_paragraph = NULL;
    n->ref = cu_str_empty();
    n->index = kind == MARK_FOOTNOTE ? -1 : 0; /* ast.NewFootnote */
    n->ref_count = 0;
    n->ref_index = 0;
    n->count = 0;
    return n;
}

/* text.NewSegments */
CuVec *mark_segments_new(CuArena *a)
{
    CuVec *v = CU_PUSH(a, CuVec, 1);

    if (v == NULL) return NULL;
    CU_VEC_INIT(v, a, MarkSegment);
    return v;
}

/* BaseBlock.Lines */
CuVec *mark_node_lines(CuArena *a, MarkNode *n)
{
    if (n->lines == NULL) n->lines = mark_segments_new(a);
    return n->lines;
}

/* Node.IsRaw: CodeBlock, FencedCodeBlock, HTMLBlock, and a Text or String
 * with the raw flag. */
int mark_node_is_raw(const MarkNode *n)
{
    switch (n->kind) {
    case MARK_CODE_BLOCK:
    case MARK_FENCED_CODE_BLOCK:
    case MARK_HTML_BLOCK:
        return 1;
    case MARK_TEXT:
    case MARK_STRING:
        return (n->flags & MARK_TEXT_RAW) != 0;
    default:
        return 0;
    }
}

/* ast.ensureIsolated */
static void ensure_isolated(MarkNode *v)
{
    if (v->parent != NULL) mark_node_remove_child(v->parent, v);
}

/* BaseNode.AppendChild */
void mark_node_append_child(MarkNode *n, MarkNode *v)
{
    ensure_isolated(v);
    if (n->first == NULL) {
        n->first = v;
        v->next = NULL;
        v->prev = NULL;
    } else {
        MarkNode *last = n->last;

        last->next = v;
        v->prev = last;
    }
    v->parent = n;
    n->last = v;
    n->child_count++;
}

/* BaseNode.RemoveChild */
void mark_node_remove_child(MarkNode *n, MarkNode *v)
{
    MarkNode *prev;
    MarkNode *next;

    if (v->parent != n) return;
    n->child_count--;
    prev = v->prev;
    next = v->next;
    if (prev != NULL) prev->next = next;
    else n->first = next;
    if (next != NULL) next->prev = prev;
    else n->last = prev;
    v->parent = NULL;
    v->prev = NULL;
    v->next = NULL;
}

/* BaseNode.InsertBefore. As in Go, the count is bumped before the nil
 * check, so inserting before a node that is not a child over-counts; the
 * count is never read by the renderer or the parsers' logic that matters. */
void mark_node_insert_before(MarkNode *n, MarkNode *v1, MarkNode *insertee)
{
    n->child_count++;
    if (v1 == NULL) {
        mark_node_append_child(n, insertee);
        return;
    }
    ensure_isolated(insertee);
    if (v1->parent == n) {
        MarkNode *prev = v1->prev;

        if (prev != NULL) {
            prev->next = insertee;
            insertee->prev = prev;
        } else {
            n->first = insertee;
            insertee->prev = NULL;
        }
        insertee->next = v1;
        v1->prev = insertee;
        insertee->parent = n;
    }
}

/* BaseNode.InsertAfter */
void mark_node_insert_after(MarkNode *n, MarkNode *v1, MarkNode *insertee)
{
    mark_node_insert_before(n, v1->next, insertee);
}

/* BaseNode.ReplaceChild */
void mark_node_replace_child(MarkNode *n, MarkNode *v1, MarkNode *insertee)
{
    mark_node_insert_before(n, v1, insertee);
    mark_node_remove_child(n, v1);
}

/* BaseNode.SetAttribute */
void mark_node_set_attribute(MarkNode *n, CuStr name, MarkAttrValue value)
{
    size_t i;
    MarkAttr a;

    for (i = 0; i < cu_vec_len(&n->attrs); i++) {
        MarkAttr *at = CU_VEC_AT(&n->attrs, MarkAttr, i);

        if (cu_str_eq(at->name, name)) {
            at->name = name;
            at->value = value;
            return;
        }
    }
    a.name = name;
    a.value = value;
    CU_VEC_PUSH(&n->attrs, &a);
}

/* BaseNode.Attribute */
const MarkAttr *mark_node_attribute(const MarkNode *n, CuStr name)
{
    size_t i;

    for (i = 0; i < cu_vec_len(&n->attrs); i++) {
        const MarkAttr *at = CU_VEC_AT(&n->attrs, MarkAttr, i);

        if (cu_str_eq(at->name, name)) return at;
    }
    return NULL;
}

int mark_node_has_attributes(const MarkNode *n)
{
    return cu_vec_len(&n->attrs) != 0;
}

MarkAttrValue mark_attr_string(CuStr s)
{
    MarkAttrValue v;

    v.kind = MARK_ATTR_STRING;
    v.str = s;
    return v;
}

MarkAttrValue mark_attr_other(MarkAttrKind kind)
{
    MarkAttrValue v;

    v.kind = kind;
    v.str = cu_str_empty();
    return v;
}

/* ast.NewTextSegment */
MarkNode *mark_text_new(CuArena *a, MarkSegment s)
{
    MarkNode *n = mark_node_new(a, MARK_TEXT);

    if (n != NULL) n->segment = s;
    return n;
}

/* ast.NewRawTextSegment */
MarkNode *mark_raw_text_new(CuArena *a, MarkSegment s)
{
    MarkNode *n = mark_text_new(a, s);

    if (n != NULL) n->flags |= MARK_TEXT_RAW;
    return n;
}

static int is_text_without_soft_break(const MarkNode *t)
{
    return t != NULL && t->kind == MARK_TEXT && (t->flags & MARK_TEXT_SOFT_LINE_BREAK) == 0;
}

/* ast.MergeOrAppendTextSegment */
int mark_merge_or_append_text(CuArena *a, MarkNode *parent, MarkSegment s)
{
    MarkNode *t = parent->last;

    if (is_text_without_soft_break(t) && t->segment.stop == s.start) {
        t->segment = mark_segment_with_stop(t->segment, s.stop);
        return 1;
    }
    t = mark_text_new(a, s);
    if (t == NULL) return 0;
    mark_node_append_child(parent, t);
    return 1;
}

/* ast.MergeOrReplaceTextSegment */
int mark_merge_or_replace_text(CuArena *a, MarkNode *parent, MarkNode *n, MarkSegment s)
{
    MarkNode *t = n->prev;

    if (is_text_without_soft_break(t) && t->segment.stop == s.start) {
        t->segment = mark_segment_with_stop(t->segment, s.stop);
        mark_node_remove_child(parent, n);
        return 1;
    }
    t = mark_text_new(a, s);
    if (t == NULL) return 0;
    mark_node_replace_child(parent, n, t);
    return 1;
}

/* ast.NewImage */
MarkNode *mark_image_new(CuArena *a, MarkNode *link)
{
    MarkNode *c = mark_node_new(a, MARK_IMAGE);
    MarkNode *n;

    if (c == NULL) return NULL;
    c->destination = link->destination;
    c->title = link->title;
    c->has_title = link->has_title;
    for (n = link->first; n != NULL;) {
        MarkNode *next = n->next;

        mark_node_remove_child(link, n);
        mark_node_append_child(c, n);
        n = next;
    }
    return c;
}

/* List.IsOrdered */
int mark_list_is_ordered(const MarkNode *list)
{
    return list->marker == '.' || list->marker == ')';
}

/* FencedCodeBlock.Language: the info string up to its first space. */
CuStr mark_fenced_language(MarkNode *n, const unsigned char *src, long len, int *has)
{
    if (!n->has_language && n->info != NULL) {
        MarkLine info = mark_segment_value(n->info->segment, src, len);
        long i = 0;
        long l = mark_line_len(info);

        for (; i < l; i++) {
            if (mark_line_at(info, i) == ' ') break;
        }
        /* Info segments have no padding, so this is a view. */
        n->language = cu_str_n((const char *)info.bytes, (size_t)(i > info.n ? info.n : i));
        n->has_language = 1;
    }
    *has = n->has_language;
    return n->language;
}

long mark_node_depth(const MarkNode *n)
{
    long d = 0;

    for (n = n->parent; n != NULL; n = n->parent) d++;
    return d;
}

/* BaseNode.SortChildren, an insertion sort over the sibling links, with
 * footnoteASTTransformer.Transform's comparator: -1 when n1.Index <
 * n2.Index, else 1. */
void mark_node_sort_children_by_index(MarkNode *n)
{
    MarkNode *sorted = NULL;
    MarkNode *current = n->first;
    MarkNode *c;

    while (current != NULL) {
        MarkNode *next = current->next;

        if (sorted == NULL || !(sorted->index < current->index)) {
            current->next = sorted;
            if (sorted != NULL) sorted->prev = current;
            sorted = current;
            sorted->prev = NULL;
        } else {
            c = sorted;
            while (c->next != NULL && c->next->index < current->index) c = c->next;
            current->next = c->next;
            current->prev = c;
            if (c->next != NULL) c->next->prev = current;
            c->next = current;
        }
        current = next;
    }
    n->first = sorted;
    for (c = n->first; c != NULL; c = c->next) n->last = c;
}

/* AutoLink.URL */
CuStr mark_autolink_url(CuArena *a, const MarkNode *n, const unsigned char *src, long len)
{
    MarkLine v;
    CuStr value;
    CuBuilder b;
    CuStr out;

    if (n->autolink_value == NULL) return cu_str_empty();
    v = mark_segment_value(n->autolink_value->segment, src, len);
    value = mark_line_slice(a, v, 0, mark_line_len(v));
    if (n->protocol.len == 0) return value;
    cu_sb_init(&b, a);
    cu_sb_str(&b, n->protocol);
    cu_sb_str(&b, CU_LIT("://"));
    cu_sb_str(&b, value);
    if (!cu_sb_finish(&b, &out)) return cu_str_empty();
    return out;
}
