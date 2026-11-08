/* goldmark's block-level extensions as C: extension/table.go (the paragraph
 * transformer and the AST transformer), extension/definition_list.go (both
 * block parsers) and extension/footnote.go (the block parser and the AST
 * transformer). Each function names the Go function it ports. The four
 * regular expressions of table.go are written out as matchers that quote
 * the expression they stand for. */
#include "mark_parser.h"
#include "mark_util.h"

static MarkNode *node_new(MarkCtx *ctx, MarkKind kind)
{
    MarkNode *n = mark_node_new(ctx->arena, kind);

    if (n == NULL) ctx->failed = 1;
    return n;
}

/* A segment clamped to the source, where Go would panic slicing past it
 * (table.go computes cell positions from a padded line, so a cell on the
 * last line of the input can reach past the source). */
static MarkSegment clamp_segment(const MarkCtx *ctx, MarkSegment s)
{
    if (s.start > ctx->src_len) s.start = ctx->src_len;
    if (s.stop > ctx->src_len) s.stop = ctx->src_len;
    if (s.start < 0) s.start = 0;
    if (s.stop < s.start) s.stop = s.start;
    return s;
}

/* util.FindClosure with codeSpan and allowNesting false, over line from
 * index from: the index (relative to from) of the closer, or -1. */
static long find_closure(MarkLine line, long from, unsigned char opener, unsigned char closer)
{
    long len = mark_line_len(line) - from;
    long i = 0;
    long opened = 1;

    while (i < len) {
        unsigned char c = mark_line_at(line, from + i);

        if (c == '\\' && i < len - 1 && mark_byte_punct(mark_line_at(line, from + i + 1))) {
            i += 2;
            continue;
        }
        if (c == closer) {
            opened--;
            if (opened == 0) return i;
        } else if (c == opener) {
            return -1; /* !allowNesting */
        }
        i++;
    }
    return -1;
}

/* ---- table.go ---- */

/* isTableDelim */
static int is_table_delim(MarkLine line)
{
    long pos;
    long len = mark_line_len(line);
    long i;

    if (mark_line_indent_width(line, 0, &pos) > 3) return 0;
    for (i = 0; i < len; i++) {
        unsigned char b = mark_line_at(line, i);

        if (!(mark_byte_space(b) || b == '-' || b == '|' || b == ':')) return 0;
    }
    return 1;
}

/* RE2's \s: [\t\n\f\r ] */
static int re_space(unsigned char c)
{
    return c == '\t' || c == '\n' || c == 0x0C || c == '\r' || c == ' ';
}

/* The alignment of one delimiter column line[from, to), by the first of
 * tableDelimLeft ^\s*\:\-+\s*$, tableDelimRight ^\s*\-+\:\s*$,
 * tableDelimCenter ^\s*\:\-+\:\s*$ and tableDelimNone ^\s*\-+\s*$ that
 * matches, or 0. The four share the shape \s* [:]? -+ [:]? \s* and differ
 * only in which colons are present, so one scan decides. */
static int delimiter_alignment(MarkLine line, long from, long to)
{
    long i = from;
    int left = 0;
    int right = 0;
    long dashes = 0;

    while (i < to && re_space(mark_line_at(line, i))) i++;
    if (i < to && mark_line_at(line, i) == ':') {
        left = 1;
        i++;
    }
    while (i < to && mark_line_at(line, i) == '-') {
        dashes++;
        i++;
    }
    if (dashes == 0) return 0;
    if (i < to && mark_line_at(line, i) == ':') {
        right = 1;
        i++;
    }
    while (i < to && re_space(mark_line_at(line, i))) i++;
    if (i != to) return 0;
    if (left && !right) return MARK_ALIGN_LEFT;
    if (!left && right) return MARK_ALIGN_RIGHT;
    if (left && right) return MARK_ALIGN_CENTER;
    return MARK_ALIGN_NONE;
}

static int range_is_blank(MarkLine line, long from, long to)
{
    return mark_line_is_blank(mark_line_sub(line, from, to));
}

/* tableParagraphTransformer.parseDelimiter: the alignments (a CuVec of
 * int), or NULL for Go's nil (not a delimiter row, or no columns). */
static CuVec *parse_delimiter(MarkCtx *ctx, MarkSegment segment)
{
    MarkLine line = mark_segment_value(segment, ctx->src, ctx->src_len);
    long len = mark_line_len(line);
    long ncols = 1;
    long first = 0;
    long last;
    long i;
    long k;
    long from;
    CuVec *alignments;

    if (!is_table_delim(line)) return NULL;
    /* bytes.Split(line, "|"): ncols pieces, blank first and last dropped */
    for (i = 0; i < len; i++) {
        if (mark_line_at(line, i) == '|') ncols++;
    }
    last = ncols - 1;
    for (i = 0; i < len && mark_line_at(line, i) != '|'; i++) {
    }
    if (range_is_blank(line, 0, i)) first = 1;
    if (last >= first) {
        for (i = len; i > 0 && mark_line_at(line, i - 1) != '|'; i--) {
        }
        if (range_is_blank(line, i, len)) last--;
    }
    if (last < first) return NULL;
    alignments = CU_PUSH(ctx->arena, CuVec, 1);
    if (alignments == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    CU_VEC_INIT(alignments, ctx->arena, int);
    from = 0;
    for (k = 0; k <= last; k++) {
        long to = from;
        int a;

        while (to < len && mark_line_at(line, to) != '|') to++;
        if (k >= first) {
            a = delimiter_alignment(line, from, to);
            if (a == 0) return NULL;
            CU_VEC_PUSH(alignments, &a);
            if (cu_vec_failed(alignments)) {
                ctx->failed = 1;
                return NULL;
            }
        }
        from = to + 1;
    }
    return alignments;
}

/* escapedPipeCell: made on the first escaped pipe of a cell and listed in
 * the context for tableASTTransformer. */
static MarkEscapedCell *escaped_cell_new(MarkCtx *ctx, MarkNode *cell)
{
    MarkEscapedCell *e = CU_PUSH(ctx->arena, MarkEscapedCell, 1);

    if (e == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    e->cell = cell;
    CU_VEC_INIT(&e->pos, ctx->arena, long);
    e->transformed = 0;
    CU_VEC_PUSH(&ctx->escaped_pipe_cells, &e);
    if (cu_vec_failed(&ctx->escaped_pipe_cells)) ctx->failed = 1;
    return e;
}

/* tableParagraphTransformer.parseRow. Positions are segment.Start plus an
 * index into the padded line, as Go computes them (off by the padding when
 * the line has any). */
static MarkNode *parse_row(MarkCtx *ctx, MarkSegment segment, CuVec *alignments, int is_header)
{
    MarkLine line = mark_segment_value(segment, ctx->src, ctx->src_len);
    long len = mark_line_len(line);
    long pos = mark_line_trim_left_space_len(line);
    long limit = len - mark_line_trim_right_space_len(line);
    long nalign = (long)cu_vec_len(alignments);
    MarkNode *row = node_new(ctx, MARK_TABLE_ROW);
    long i = 0;

    if (row == NULL) return NULL;
    row->alignments = alignments;
    /* Go indexes line[pos] and line[limit-1] unguarded; a paragraph line
     * is never blank, so both are in range, and they are guarded here. */
    if (len > 0 && pos < len && mark_line_at(line, pos) == '|') pos++;
    if (len > 0 && limit > 0 && mark_line_at(line, limit - 1) == '|') limit--;
    for (; pos < limit; i++) {
        int alignment = MARK_ALIGN_NONE;
        MarkEscapedCell *escaped = NULL;
        MarkNode *cell;
        int has_backtick = 0;
        long closure = pos;
        MarkSegment seg;
        CuVec *lines;

        if (i >= nalign) {
            if (!is_header) return row;
        } else {
            alignment = *CU_VEC_AT(alignments, int, (size_t)i);
        }
        cell = node_new(ctx, MARK_TABLE_CELL);
        if (cell == NULL) return NULL;
        cell->alignment = alignment;
        for (; closure < limit; closure++) {
            unsigned char c = mark_line_at(line, closure);

            if (c == '`') has_backtick = 1;
            if (c == '|') {
                if (closure == 0 || mark_line_at(line, closure - 1) != '\\') break;
                if (has_backtick) {
                    long p = segment.start + closure - 1;

                    if (escaped == NULL) {
                        escaped = escaped_cell_new(ctx, cell);
                        if (escaped == NULL) return NULL;
                    }
                    CU_VEC_PUSH(&escaped->pos, &p);
                    if (cu_vec_failed(&escaped->pos)) {
                        ctx->failed = 1;
                        return NULL;
                    }
                }
            }
        }
        seg = clamp_segment(ctx, mark_segment(segment.start + pos, segment.start + closure));
        seg = mark_segment_trim_left_space(seg, ctx->src, ctx->src_len);
        seg = mark_segment_trim_right_space(seg, ctx->src, ctx->src_len);
        lines = mark_node_lines(ctx->arena, cell);
        mark_lines_append(ctx, lines, seg);
        if (ctx->failed) return NULL;
        mark_node_append_child(row, cell);
        pos = closure + 1;
    }
    for (; i < nalign; i++) {
        MarkNode *cell = node_new(ctx, MARK_TABLE_CELL);

        if (cell == NULL) return NULL;
        mark_node_append_child(row, cell);
    }
    return row;
}

/* ast.NewTableHeader: takes the row's cells. */
static MarkNode *table_header_new(MarkCtx *ctx, MarkNode *row)
{
    MarkNode *n = node_new(ctx, MARK_TABLE_HEADER);
    MarkNode *c;

    if (n == NULL) return NULL;
    for (c = row->first; c != NULL;) {
        MarkNode *next = c->next;

        mark_node_append_child(n, c);
        c = next;
    }
    return n;
}

/* tableParagraphTransformer.Transform */
void mark_table_transform(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    CuVec *lines = mark_node_lines(ctx->arena, node);
    long i;

    (void)r;
    if (lines == NULL) {
        ctx->failed = 1;
        return;
    }
    if (cu_vec_len(lines) < 2) return;
    for (i = 1; i < (long)cu_vec_len(lines); i++) {
        CuVec *alignments = parse_delimiter(ctx, mark_lines_at(lines, i));
        MarkNode *header;
        MarkNode *table;
        MarkNode *head;
        long j;

        if (ctx->failed) return;
        if (alignments == NULL) continue;
        header = parse_row(ctx, mark_lines_at(lines, i - 1), alignments, 1);
        if (header == NULL) return;
        if ((long)cu_vec_len(alignments) != header->child_count) return;
        table = node_new(ctx, MARK_TABLE);
        head = table_header_new(ctx, header);
        if (table == NULL || head == NULL) return;
        table->alignments = alignments;
        mark_node_append_child(table, head);
        for (j = i + 1; j < (long)cu_vec_len(lines); j++) {
            MarkNode *row = parse_row(ctx, mark_lines_at(lines, j), alignments, 0);

            if (row == NULL) return;
            mark_node_append_child(table, row);
        }
        mark_lines_truncate(lines, (size_t)(i - 1));
        if (node->parent == NULL) return; /* Go would dereference nil */
        mark_node_insert_after(node->parent, node, table);
        if (cu_vec_len(lines) == 0) {
            mark_node_remove_child(node->parent, node);
        } else {
            MarkSegment *last = CU_VEC_AT(lines, MarkSegment, (size_t)(i - 2));

            last->stop--; /* trim last newline(\n) */
        }
    }
}

/* The splitting of one Text child of a code span at every listed escaped
 * pipe inside it (the inner loops of tableASTTransformer.Transform). */
static void split_text_at_pipes(MarkCtx *ctx, MarkNode *parent, MarkNode *c)
{
    MarkSegment ts = c->segment;
    MarkNode *n = c;
    size_t v;

    for (v = 0; v < cu_vec_len(&ctx->escaped_pipe_cells); v++) {
        MarkEscapedCell *e = *CU_VEC_AT(&ctx->escaped_pipe_cells, MarkEscapedCell *, v);
        size_t k;

        for (k = 0; k < cu_vec_len(&e->pos); k++) {
            long pos = *CU_VEC_AT(&e->pos, long, k);

            if (ts.start <= pos && pos < ts.stop) {
                MarkSegment segment = n->segment;
                MarkNode *n1 = mark_raw_text_new(ctx->arena, mark_segment_with_stop(segment, pos));
                MarkNode *n2 = mark_raw_text_new(ctx->arena, mark_segment_with_start(segment, pos + 1));

                if (n1 == NULL || n2 == NULL) {
                    ctx->failed = 1;
                    return;
                }
                mark_node_insert_after(parent, n, n1);
                mark_node_insert_after(parent, n1, n2);
                mark_node_remove_child(parent, n);
                n = n2;
                e->transformed = 1;
            }
        }
    }
}

/* tableASTTransformer.Transform: in every cell that had an escaped pipe
 * after a backtick, the Text children of code spans are split around the
 * pipe's backslash, which drops it. The walk of each cell is iterative. */
void mark_table_ast_transform(MarkCtx *ctx, MarkNode *doc)
{
    size_t v;

    (void)doc;
    if (cu_vec_len(&ctx->escaped_pipe_cells) == 0) return; /* lst == nil */
    for (v = 0; v < cu_vec_len(&ctx->escaped_pipe_cells); v++) {
        MarkEscapedCell *e = *CU_VEC_AT(&ctx->escaped_pipe_cells, MarkEscapedCell *, v);
        MarkNode *root = e->cell;
        MarkNode *n = root;

        if (e->transformed) continue;
        for (;;) {
            if (ctx->failed) return;
            if (n->kind == MARK_CODE_SPAN) {
                MarkNode *c;

                for (c = n->first; c != NULL;) {
                    MarkNode *next = c->next;

                    if (c->kind == MARK_TEXT) split_text_at_pipes(ctx, c->parent, c);
                    c = next;
                }
                /* the children are leaves; Go visits them to no effect */
            } else if (n->first != NULL) {
                n = n->first;
                continue;
            }
            while (n != root && n->next == NULL) n = n->parent;
            if (n == root) break;
            n = n->next;
        }
    }
    cu_vec_clear(&ctx->escaped_pipe_cells); /* pc.Set(escapedPipeCellListKey, nil) */
}

/* ---- definition_list.go ---- */

/* definitionListParser.Open */
MarkNode *mark_definition_list_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment seg;
    MarkLine line;
    long pos = ctx->block_offset;
    long indent = ctx->block_indent;
    MarkNode *last;
    long w;
    long wpos;
    MarkNode *list;

    *state = MARK_STATE_NO_CHILDREN;
    if (parent->kind == MARK_DEFINITION_LIST) return NULL;
    line = mark_reader_peek_line(r, &seg);
    if (pos < 0 || mark_line_at(line, pos) != ':' || indent != 0) return NULL;
    last = parent->last;
    /* need 1 or more spaces after ':' */
    w = mark_line_indent_width(mark_line_skip(line, pos + 1), pos + 1, &wpos);
    if (w < 1) return NULL;
    if (w >= 8) w = 5; /* starts with indented code */
    w += pos + 1;      /* 1 = ':' */
    *state = MARK_STATE_HAS_CHILDREN;
    if (last != NULL && last->kind == MARK_PARAGRAPH) {
        if (last->prev != NULL && last->prev->kind == MARK_DEFINITION_LIST) { /* is not first item */
            list = last->prev;
            list->offset = w;
            list->temporary_paragraph = last;
        } else { /* is first item */
            list = node_new(ctx, MARK_DEFINITION_LIST);
            if (list == NULL) return NULL;
            list->offset = w;
            list->temporary_paragraph = last;
            *state |= MARK_STATE_REQUIRE_PARAGRAPH;
        }
    } else if (last != NULL && last->kind == MARK_DEFINITION_LIST) { /* multiple description */
        list = last;
        list->offset = w;
        list->temporary_paragraph = NULL;
    } else {
        *state = MARK_STATE_NO_CHILDREN;
        return NULL;
    }
    return list;
}

/* definitionListParser.Continue */
unsigned mark_definition_list_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long w;
    long pos;
    long padding;

    (void)ctx;
    if (mark_line_is_blank(line)) return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
    w = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);
    if (w < node->offset) return MARK_STATE_CLOSE;
    pos = mark_line_indent_position(line, mark_reader_line_offset(r), 0, node->offset, &padding);
    mark_reader_advance_and_set_padding(r, pos, padding);
    return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
}

/* definitionDescriptionParser.Open */
MarkNode *mark_definition_description_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment seg;
    MarkLine line;
    long pos = ctx->block_offset;
    long indent = ctx->block_indent;
    MarkNode *para;
    long cpos;
    long padding;
    MarkNode *desc;

    *state = MARK_STATE_NO_CHILDREN;
    line = mark_reader_peek_line(r, &seg);
    if (pos < 0 || mark_line_at(line, pos) != ':' || indent != 0) return NULL;
    if (parent->kind != MARK_DEFINITION_LIST) return NULL;
    para = parent->temporary_paragraph;
    parent->temporary_paragraph = NULL;
    if (para != NULL) {
        CuVec *lines = mark_node_lines(ctx->arena, para);
        size_t l;
        size_t i;

        if (lines == NULL) {
            ctx->failed = 1;
            return NULL;
        }
        l = cu_vec_len(lines);
        for (i = 0; i < l; i++) {
            MarkNode *term = node_new(ctx, MARK_DEFINITION_TERM);
            MarkSegment segment = mark_lines_at(lines, (long)i);

            if (term == NULL) return NULL;
            mark_lines_append(ctx, mark_node_lines(ctx->arena, term),
                              mark_segment_trim_right_space(segment, ctx->src, ctx->src_len));
            if (ctx->failed) return NULL;
            mark_node_append_child(parent, term);
        }
        if (para->parent != NULL) mark_node_remove_child(para->parent, para); /* Go would dereference nil */
    }
    cpos = mark_line_indent_position(mark_line_skip(line, pos + 1), pos + 1, 0, parent->offset - pos - 1, &padding);
    mark_reader_advance_and_set_padding(r, cpos + 1, padding);
    desc = node_new(ctx, MARK_DEFINITION_DESCRIPTION);
    if (desc == NULL) return NULL;
    *state = MARK_STATE_HAS_CHILDREN;
    return desc;
}

/* definitionDescriptionParser.Continue: the list parser closes the
 * description, so this only continues. */
unsigned mark_definition_description_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)ctx;
    (void)node;
    (void)r;
    return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
}

/* definitionDescriptionParser.Close. goldmark's loop steps to the
 * replaced paragraph's NextSibling, which RemoveChild has just cleared, so
 * only the first paragraph of a tight description becomes a text block;
 * any later paragraph in it still renders as <p>. */
void mark_definition_description_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkNode *gc;

    (void)r;
    node->is_tight = !node->blank_previous_lines;
    if (!node->is_tight) return;
    for (gc = node->first; gc != NULL; gc = gc->next) {
        if (gc->kind == MARK_PARAGRAPH) {
            MarkNode *text_block = node_new(ctx, MARK_TEXT_BLOCK);

            if (text_block == NULL) return;
            text_block->lines = gc->lines;
            mark_node_replace_child(node, gc, text_block);
            break;
        }
    }
}

/* ---- footnote.go (blocks) ---- */

/* footnoteBlockParser.Open */
MarkNode *mark_footnote_block_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    long len = mark_line_len(line);
    long pos = ctx->block_offset;
    long open;
    long closure;
    long closes;
    long next;
    long padding;
    CuStr label;
    MarkLine lv;
    MarkNode *item;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (pos < 0 || mark_line_at(line, pos) != '[') return NULL;
    pos++;
    if (pos > len - 1 || mark_line_at(line, pos) != '^') return NULL;
    open = pos + 1;
    closure = find_closure(line, pos + 1, '[', ']');
    closes = pos + 1 + closure;
    next = closes + 1;
    if (closure > -1) {
        if (next >= len || mark_line_at(line, next) != ':') return NULL;
    } else {
        return NULL;
    }
    padding = segment.padding;
    lv = mark_segment_value(clamp_segment(ctx, mark_segment(segment.start + open - padding, segment.start + closes - padding)),
                            ctx->src, ctx->src_len);
    label = mark_line_slice(ctx->arena, lv, 0, mark_line_len(lv));
    if (ctx->failed) return NULL;
    if (mark_is_blank((const unsigned char *)label.ptr, (long)label.len)) return NULL;
    item = node_new(ctx, MARK_FOOTNOTE);
    if (item == NULL) return NULL;
    item->ref = label;

    pos = next + 1 - padding;
    if (pos >= len) {
        mark_reader_advance(r, pos);
        return item;
    }
    mark_reader_advance_and_set_padding(r, pos, padding);
    *state = MARK_STATE_HAS_CHILDREN;
    return item;
}

/* footnoteBlockParser.Continue */
unsigned mark_footnote_block_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long childpos;
    long padding;

    (void)ctx;
    (void)node;
    if (mark_line_is_blank(line)) return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
    childpos = mark_line_indent_position(line, mark_reader_line_offset(r), 0, 4, &padding);
    if (childpos < 0) return MARK_STATE_CLOSE;
    mark_reader_advance_and_set_padding(r, childpos, padding);
    return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
}

/* footnoteBlockParser.Close: moves the footnote into the document's
 * footnote list, made before the first footnote in its parent. */
void mark_footnote_block_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkNode *list = ctx->footnote_list;

    (void)r;
    if (node->parent == NULL) return; /* Go would dereference nil */
    if (list == NULL) {
        list = node_new(ctx, MARK_FOOTNOTE_LIST);
        if (list == NULL) return;
        ctx->footnote_list = list;
        mark_node_insert_before(node->parent, node, list);
    }
    mark_node_remove_child(node->parent, node);
    mark_node_append_child(list, node);
}

/* footnoteASTTransformer.Transform: numbers the references of each
 * footnote, appends its backlinks, drops unreferenced footnotes, sorts the
 * rest by index and moves the list to the end of the document. */
void mark_footnote_ast_transform(MarkCtx *ctx, MarkNode *doc)
{
    MarkNode *list = ctx->footnote_list;
    size_t nlinks = cu_vec_len(&ctx->footnote_links);
    long *counter;
    long *ref_counter;
    long k;
    size_t i;
    MarkNode *footnote;

    ctx->footnote_list = NULL;
    if (list == NULL) {
        cu_vec_clear(&ctx->footnote_links);
        return;
    }
    /* counter and refCounter are maps keyed by index; every index is in
     * 1..Count (footnoteParser.Parse assigns them), so arrays serve */
    counter = CU_PUSH(ctx->arena, long, (size_t)list->count + 1);
    ref_counter = CU_PUSH(ctx->arena, long, (size_t)list->count + 1);
    if (counter == NULL || ref_counter == NULL) {
        ctx->failed = 1;
        return;
    }
    for (k = 0; k <= list->count; k++) {
        counter[k] = 0;
        ref_counter[k] = 0;
    }
    for (i = 0; i < nlinks; i++) {
        MarkNode *fnlink = *CU_VEC_AT(&ctx->footnote_links, MarkNode *, i);

        if (fnlink->index >= 0 && fnlink->index <= list->count) counter[fnlink->index]++;
    }
    for (i = 0; i < nlinks; i++) {
        MarkNode *fnlink = *CU_VEC_AT(&ctx->footnote_links, MarkNode *, i);
        long idx = fnlink->index;

        if (idx < 0 || idx > list->count) continue; /* unreachable: Parse keeps indices in range */
        fnlink->ref_count = counter[idx];
        fnlink->ref_index = ref_counter[idx];
        ref_counter[idx]++;
    }
    cu_vec_clear(&ctx->footnote_links);
    for (footnote = list->first; footnote != NULL;) {
        MarkNode *container = footnote;
        MarkNode *next = footnote->next;
        long index = footnote->index;

        if (footnote->last != NULL && footnote->last->kind == MARK_PARAGRAPH) container = footnote->last;
        if (index < 0) {
            mark_node_remove_child(list, footnote);
        } else {
            long ref_count = index <= list->count ? counter[index] : 0;
            long ri;

            for (ri = 0; ri < (ref_count > 1 ? ref_count : 1); ri++) {
                MarkNode *back = node_new(ctx, MARK_FOOTNOTE_BACKLINK);

                if (back == NULL) return;
                back->index = index;
                back->ref_count = ref_count;
                back->ref_index = ri;
                mark_node_append_child(container, back);
            }
        }
        footnote = next;
    }
    mark_node_sort_children_by_index(list);
    if (list->count <= 0) {
        if (list->parent != NULL) mark_node_remove_child(list->parent, list);
        return;
    }
    mark_node_append_child(doc, list);
}
