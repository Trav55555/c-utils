/* goldmark parser/parser.go as C: the parse context, auto heading ids, the
 * block driver (parseBlocks, openBlocks, closeBlocks) and the inline driver
 * (parseBlock). Each function names the Go function it ports. */
#include "mark_parser.h"
#include "mark_util.h"

/* ---- Context (parser.parseContext) ---- */

void mark_ctx_init(MarkCtx *ctx, CuArena *a, CuStr src, unsigned long flags)
{
    ctx->arena = a;
    ctx->src = (const unsigned char *)src.ptr;
    ctx->src_len = (long)src.len;
    ctx->flags = flags;
    ctx->failed = 0;
    ctx->limit = 0;
    CU_MAP_INIT(&ctx->refs, a, MarkReference);
    CU_MAP_INIT(&ctx->ids, a, int);
    ctx->block_offset = -1;
    ctx->block_indent = -1;
    ctx->delimiters = NULL;
    ctx->last_delimiter = NULL;
    CU_VEC_INIT(&ctx->opened, a, MarkBlock);
    ctx->temporary_paragraph = NULL;
    ctx->skip_list_parser = 0;
    ctx->empty_list_item_with_blank_lines = 0;
    ctx->fence_set = 0;
    ctx->fence_char = 0;
    ctx->fence_indent = 0;
    ctx->fence_length = 0;
    ctx->fence_node = NULL;
    ctx->link_label_state = NULL;
    ctx->link_bottom = NULL;
    ctx->link_bottom_set = 0;
}

/* parseContext.LastOpenedBlock */
MarkBlock mark_last_opened_block(const MarkCtx *ctx)
{
    size_t l = cu_vec_len(&ctx->opened);
    MarkBlock b;

    if (l != 0) return *CU_VEC_AT(&ctx->opened, MarkBlock, l - 1);
    b.node = NULL;
    b.parser = MARK_BP_PARAGRAPH;
    return b;
}

/* parseContext.AddReference: the first definition of a label wins. */
void mark_add_reference(MarkCtx *ctx, MarkReference ref)
{
    CuBuilder b;
    CuStr key;

    cu_sb_init(&b, ctx->arena);
    mark_to_link_reference(&b, ref.label);
    if (!cu_sb_finish(&b, &key)) {
        ctx->failed = 1;
        return;
    }
    if (CU_MAP_GET(&ctx->refs, MarkReference, key) == NULL) {
        if (CU_MAP_UPSERT(&ctx->refs, MarkReference, key, &ref) == NULL) ctx->failed = 1;
    }
}

/* parseContext.Reference; key is already util.ToLinkReference'd */
const MarkReference *mark_reference(MarkCtx *ctx, CuStr key)
{
    return CU_MAP_GET(&ctx->refs, MarkReference, key);
}

/* ids.Generate for ast.KindHeading. Candidates "<id>-<n>" are tried under
 * a temp scope so the ones already taken leave nothing in the arena. */
CuStr mark_ids_generate(MarkCtx *ctx, CuStr value)
{
    CuBuilder b;
    CuStr result;
    const unsigned char *s;
    size_t i;
    unsigned long n;
    int one = 1;

    value = mark_trim_spaces(value);
    s = (const unsigned char *)value.ptr;
    cu_sb_init(&b, ctx->arena);
    for (i = 0; i < value.len;) {
        unsigned char v = s[i];
        int l = mark_utf8_len(v);

        i += (size_t)l;
        if (l != 1) continue;
        if (mark_byte_alnum(v)) {
            if ('A' <= v && v <= 'Z') v += 'a' - 'A';
            cu_sb_char(&b, (char)v);
        } else if (mark_byte_space(v) || v == '-' || v == '_') {
            cu_sb_char(&b, '-');
        }
    }
    if (b.len == 0) cu_sb_str(&b, CU_LIT("heading"));
    if (!cu_sb_finish(&b, &result)) {
        ctx->failed = 1;
        return cu_str_empty();
    }
    if (CU_MAP_GET(&ctx->ids, int, result) == NULL) {
        if (CU_MAP_UPSERT(&ctx->ids, int, result, &one) == NULL) ctx->failed = 1;
        return result;
    }
    for (n = 1;; n++) {
        CuTemp t = cu_temp_begin(ctx->arena);
        CuBuilder cb;
        CuStr cand;
        int taken;

        cu_sb_init(&cb, ctx->arena);
        cu_sb_str(&cb, result);
        cu_sb_char(&cb, '-');
        cu_sb_ulong(&cb, n);
        if (!cu_sb_finish(&cb, &cand)) {
            cu_temp_end(t);
            ctx->failed = 1;
            return cu_str_empty();
        }
        taken = CU_MAP_GET(&ctx->ids, int, cand) != NULL;
        cu_temp_end(t);
        if (!taken) break;
    }
    cu_sb_init(&b, ctx->arena);
    cu_sb_str(&b, result);
    cu_sb_char(&b, '-');
    cu_sb_ulong(&b, n);
    if (!cu_sb_finish(&b, &result)) {
        ctx->failed = 1;
        return cu_str_empty();
    }
    if (CU_MAP_UPSERT(&ctx->ids, int, result, &one) == NULL) ctx->failed = 1;
    return result;
}

/* ids.Put */
void mark_ids_put(MarkCtx *ctx, CuStr id)
{
    int one = 1;

    if (CU_MAP_UPSERT(&ctx->ids, int, id, &one) == NULL) ctx->failed = 1;
}

/* ---- Line vectors (text.Segments) ---- */

void mark_lines_append(MarkCtx *ctx, CuVec *lines, MarkSegment s)
{
    if (lines == NULL) {
        ctx->failed = 1;
        return;
    }
    CU_VEC_PUSH(lines, &s);
    if (cu_vec_failed(lines)) ctx->failed = 1;
}

MarkSegment mark_lines_at(const CuVec *lines, long i)
{
    if (lines == NULL || i < 0 || (size_t)i >= cu_vec_len(lines)) return mark_segment(-1, -1);
    return *CU_VEC_AT(lines, MarkSegment, (size_t)i);
}

/* Segments.SetSliced(0, n), by popping: CuVec has no shrink call. */
void mark_lines_truncate(CuVec *lines, size_t n)
{
    MarkSegment tmp;

    while (cu_vec_len(lines) > n) {
        if (!CU_VEC_POP(lines, &tmp)) break;
    }
}

/* ---- Block parser dispatch (parser.addBlockParser) ---- */

/* The parsers for a line starting with c: those whose trigger contains c,
 * in priority order, then the trigger-less ones, as Go appends
 * freeBlockParsers after each trigger's list. */
static size_t block_parsers_for(unsigned char c, MarkBlockParserId out[MARK_BP_COUNT])
{
    size_t n = 0;
    int id;

    for (id = 0; id < MARK_BP_COUNT; id++) {
        const char *t = mark_block_parsers[id].trigger;

        if (t != NULL && c != 0 && strchr(t, (int)c) != NULL) out[n++] = (MarkBlockParserId)id;
    }
    for (id = 0; id < MARK_BP_COUNT; id++) {
        if (mark_block_parsers[id].trigger == NULL) out[n++] = (MarkBlockParserId)id;
    }
    return n;
}

static MarkBlock block_at(const MarkCtx *ctx, long i)
{
    MarkBlock b;

    if (i >= 0 && (size_t)i < cu_vec_len(&ctx->opened)) return *CU_VEC_AT(&ctx->opened, MarkBlock, (size_t)i);
    b.node = NULL;
    b.parser = MARK_BP_PARAGRAPH;
    return b;
}

/* Removes elements [from, to] of the opened blocks. */
static void blocks_remove(CuVec *v, size_t from, size_t to)
{
    size_t len = cu_vec_len(v);
    size_t count;
    size_t k;
    MarkBlock *d;
    MarkBlock tmp;

    if (from >= len) return;
    if (to >= len) to = len - 1;
    count = to - from + 1;
    d = CU_VEC_DATA(v, MarkBlock);
    if (to + 1 < len) memmove(d + from, d + to + 1, (len - to - 1) * sizeof *d);
    for (k = 0; k < count; k++) {
        if (!CU_VEC_POP(v, &tmp)) break;
    }
}

static void blocks_push(MarkCtx *ctx, MarkNode *node, MarkBlockParserId parser)
{
    MarkBlock b;

    b.node = node;
    b.parser = parser;
    CU_VEC_PUSH(&ctx->opened, &b);
    if (cu_vec_failed(&ctx->opened)) ctx->failed = 1;
}

/* parser.transformParagraph: 1 when the paragraph was replaced. */
static int transform_paragraph(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    mark_link_reference_transform(ctx, node, r);
    return node->parent == NULL;
}

/* parser.closeBlocks */
static void close_blocks(MarkCtx *ctx, long from, long to, MarkReader *r)
{
    long len = (long)cu_vec_len(&ctx->opened);
    long i;

    if (from >= len) from = len - 1;
    for (i = from; i >= to; i--) {
        MarkBlock be = block_at(ctx, i);

        if (be.node == NULL) continue;
        if (be.node->kind == MARK_PARAGRAPH && be.node->parent != NULL) transform_paragraph(ctx, be.node, r);
        if (be.node->parent != NULL) mark_block_parsers[be.parser].close(ctx, be.node, r);
    }
    if (to < 0) to = 0;
    if (from >= to) blocks_remove(&ctx->opened, (size_t)to, (size_t)from);
}

/* parser.blockOpenResult */
#define PARAGRAPH_CONTINUATION 1
#define NEW_BLOCKS_OPENED 2
#define NO_BLOCKS_OPENED 3

/* parser.openBlocks */
static int open_blocks(MarkCtx *ctx, MarkNode *parent, int blank_line, MarkReader *r)
{
    int result = NO_BLOCKS_OPENED;
    int continuable = 0;
    MarkBlock last_block = mark_last_opened_block(ctx);

    if (last_block.node != NULL) continuable = last_block.node->kind == MARK_PARAGRAPH;
retry:
    {
        MarkSegment seg;
        MarkLine line = mark_reader_peek_line(r, &seg);
        long len = mark_line_len(line);
        long pos;
        long w = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);
        MarkBlockParserId bps[MARK_BP_COUNT];
        size_t nbps;
        size_t k;

        if (w >= len) {
            ctx->block_offset = -1;
            ctx->block_indent = -1;
        } else {
            ctx->block_offset = pos;
            ctx->block_indent = w;
        }
        if (!line.valid || mark_line_at(line, 0) == '\n') goto continuable_check;
        nbps = block_parsers_for(pos < len ? mark_line_at(line, pos) : 0, bps);
        for (k = 0; k < nbps; k++) {
            const MarkBlockParser *bp = &mark_block_parsers[bps[k]];
            MarkNode *last;
            MarkNode *node;
            unsigned state = MARK_STATE_NO_CHILDREN;

            if (continuable && result == NO_BLOCKS_OPENED && !bp->can_interrupt_paragraph) continue;
            if (w > 3 && !bp->can_accept_indented_line) continue;
            last_block = mark_last_opened_block(ctx);
            last = last_block.node;
            node = bp->open(ctx, parent, r, &state);
            if (ctx->failed) return result;
            if (node == NULL) continue;
            if (state & MARK_STATE_REQUIRE_PARAGRAPH) {
                /* The setext parser asks for the paragraph to be closed
                 * first; when a transformer then replaces it, this parser
                 * is taken to have failed and the line is retried. */
                if (last != NULL && last == parent->last) {
                    size_t l = cu_vec_len(&ctx->opened);

                    mark_block_parsers[last_block.parser].close(ctx, last, r);
                    if (l != 0) blocks_remove(&ctx->opened, l - 1, l - 1);
                    if (transform_paragraph(ctx, last, r)) {
                        continuable = 0;
                        goto retry;
                    }
                }
            }
            node->blank_previous_lines = blank_line;
            if (last != NULL && last->parent == NULL) {
                long last_pos = (long)cu_vec_len(&ctx->opened) - 1;

                close_blocks(ctx, last_pos, last_pos, r);
            }
            /* The bound of cu_mark.h: a container (the only kind of block
             * that is given children, so the only one that nests) may
             * have at most CU_MARK_MAX_DEPTH containers above it. */
            if ((state & MARK_STATE_HAS_CHILDREN) && mark_node_depth(parent) + 1 > CU_MARK_MAX_DEPTH) {
                ctx->limit = 1;
                return result;
            }
            mark_node_append_child(parent, node);
            result = NEW_BLOCKS_OPENED;
            blocks_push(ctx, node, bps[k]);
            if (ctx->failed) return result;
            if (state & MARK_STATE_HAS_CHILDREN) {
                parent = node;
                goto retry;
            }
            break;
        }
    }
continuable_check:
    if (result == NO_BLOCKS_OPENED && continuable && last_block.node != NULL) {
        unsigned state = mark_block_parsers[last_block.parser].cont(ctx, last_block.node, r);

        if (state & MARK_STATE_CONTINUE) result = PARAGRAPH_CONTINUATION;
    }
    return result;
}

/* parser.lineStat */
typedef struct LineStat {
    long line_num;
    long level;
    int is_blank;
} LineStat;

/* parser.isBlankLine */
static int is_blank_line(long line_num, long level, const CuVec *stats)
{
    int ret = 1;
    long i;

    for (i = (long)cu_vec_len(stats) - 1 - level; i >= 0; i--) {
        const LineStat *s = CU_VEC_AT(stats, LineStat, (size_t)i);

        ret = 0;
        if (s->line_num == line_num) {
            if (s->level < level && s->is_blank) return 1;
            if (s->level == level) return s->is_blank;
        }
        if (s->line_num < line_num) return ret;
    }
    return ret;
}

static void stats_push(MarkCtx *ctx, CuVec *stats, long line_num, long level, int is_blank)
{
    LineStat s;

    s.line_num = line_num;
    s.level = level;
    s.is_blank = is_blank;
    CU_VEC_PUSH(stats, &s);
    if (cu_vec_failed(stats)) ctx->failed = 1;
}

/* parser.parseBlocks */
static void parse_blocks(MarkCtx *ctx, MarkNode *parent, MarkReader *r)
{
    CuVec blank_lines;
    int is_blank;

    CU_VEC_INIT(&blank_lines, ctx->arena, LineStat);
    cu_vec_clear(&ctx->opened);
    for (;;) {
        MarkSegment seg;
        long lines;
        long line_num;

        if (!mark_reader_skip_blank_lines(r, &seg, &lines)) return;
        line_num = r->line;
        if (lines != 0) {
            size_t l = cu_vec_len(&ctx->opened);
            size_t i;

            cu_vec_clear(&blank_lines);
            for (i = 0; i < l; i++) stats_push(ctx, &blank_lines, line_num - 1, (long)i, 1);
        }
        is_blank = is_blank_line(line_num - 1, 0, &blank_lines);
        if (open_blocks(ctx, parent, is_blank, r) != NEW_BLOCKS_OPENED) return;
        if (ctx->failed || ctx->limit) return;
        mark_reader_advance_line(r);
        for (;;) {
            long l = (long)cu_vec_len(&ctx->opened);
            long last_index;
            long i;

            if (l == 0) break;
            last_index = l - 1;
            for (i = 0; i < l; i++) {
                MarkBlock be = block_at(ctx, i);
                MarkLine line = mark_reader_peek_line(r, &seg);
                MarkNode *this_parent;
                MarkNode *last_node;
                int result;

                if (!line.valid) {
                    close_blocks(ctx, last_index, 0, r);
                    mark_reader_advance_line(r);
                    return;
                }
                line_num = r->line;
                stats_push(ctx, &blank_lines, line_num, i, mark_line_is_blank(line));
                if (ctx->failed) return;
                if (be.node->kind != MARK_PARAGRAPH) {
                    unsigned state = mark_block_parsers[be.parser].cont(ctx, be.node, r);

                    if (state & MARK_STATE_CONTINUE) {
                        if ((state & MARK_STATE_HAS_CHILDREN) && i == last_index) {
                            is_blank = is_blank_line(line_num - 1, i, &blank_lines);
                            open_blocks(ctx, be.node, is_blank, r);
                            break;
                        }
                        continue;
                    }
                }
                is_blank = is_blank_line(line_num - 1, i, &blank_lines);
                this_parent = parent;
                if (i != 0) this_parent = block_at(ctx, i - 1).node;
                last_node = block_at(ctx, last_index).node;
                result = open_blocks(ctx, this_parent, is_blank, r);
                if (result != PARAGRAPH_CONTINUATION) {
                    /* Go reads openedBlocks[lastIndex] from the slice it
                     * captured, whose backing array the new block was
                     * written into, so any change there means the last
                     * paragraph was transformed. */
                    if (block_at(ctx, last_index).node != last_node) last_index--;
                    close_blocks(ctx, last_index, i, r);
                }
                break;
            }
            if (ctx->failed || ctx->limit) return;
            mark_reader_advance_line(r);
        }
    }
}

/* ---- Inline driver (parser.parseBlock) ---- */

#define LINE_BREAK_HARD 1u
#define LINE_BREAK_SOFT 2u
#define LINE_BREAK_VISIBLE 4u

static void parse_block(MarkCtx *ctx, MarkReader *block, MarkNode *parent)
{
    CuVec *lines;
    int escaped = 0;

    if (mark_node_is_raw(parent)) return;
    lines = mark_node_lines(ctx->arena, parent);
    if (lines == NULL) {
        ctx->failed = 1;
        return;
    }
    mark_reader_reset(block, lines);
    for (;;) {
        MarkSegment seg;
        MarkLine line;
        long line_length;
        unsigned line_break_flags = 0;
        int has_new_line;
        long l;
        MarkSegment start_position;
        long n = 0;
        long i;
        long current_l;
        MarkSegment current_position;
        MarkSegment diff;
        MarkNode *text;

    retry:
        /* Go declares lineBreakFlags after its retry label, so a retry
         * starts from zero. */
        line_break_flags = 0;
        if (ctx->failed) return;
        line = mark_reader_peek_line(block, &seg);
        if (!line.valid) break;
        line_length = mark_line_len(line);
        has_new_line = line_length > 0 && mark_line_at(line, line_length - 1) == '\n';
        if (((line_length >= 3 && mark_line_at(line, line_length - 2) == '\\' &&
              mark_line_at(line, line_length - 3) != '\\') ||
             (line_length == 2 && mark_line_at(line, line_length - 2) == '\\')) &&
            has_new_line) { /* ends with \\n */
            line_length -= 2;
            line_break_flags |= LINE_BREAK_HARD | LINE_BREAK_VISIBLE;
        } else if (((line_length >= 4 && mark_line_at(line, line_length - 3) == '\\' &&
                     mark_line_at(line, line_length - 2) == '\r' && mark_line_at(line, line_length - 4) != '\\') ||
                    (line_length == 3 && mark_line_at(line, line_length - 3) == '\\' &&
                     mark_line_at(line, line_length - 2) == '\r')) &&
                   has_new_line) { /* ends with \\r\n */
            line_length -= 3;
            line_break_flags |= LINE_BREAK_HARD | LINE_BREAK_VISIBLE;
        } else if (line_length >= 3 && mark_line_at(line, line_length - 3) == ' ' &&
                   mark_line_at(line, line_length - 2) == ' ' && has_new_line) { /* two spaces, \n */
            line_length -= 3;
            line_break_flags |= LINE_BREAK_HARD;
        } else if (line_length >= 4 && mark_line_at(line, line_length - 4) == ' ' &&
                   mark_line_at(line, line_length - 3) == ' ' && mark_line_at(line, line_length - 2) == '\r' &&
                   has_new_line) { /* two spaces, \r\n */
            line_length -= 4;
            line_break_flags |= LINE_BREAK_HARD;
        } else if (has_new_line) {
            line_break_flags |= LINE_BREAK_SOFT;
        }

        mark_reader_position(block, &l, &start_position);
        n = 0;
        for (i = 0; i < line_length; i++) {
            unsigned char c = mark_line_at(line, i);
            int is_space;
            int is_punct;

            if (c == '\n') break;
            is_space = mark_byte_space(c) && c != '\r' && c != '\n';
            is_punct = mark_byte_punct(c);
            /* goldmark's escapedSpace option is off, so an escaped space
             * still triggers; Go: isSpace && !(escaped && p.escapedSpace) */
            if ((is_punct && !escaped) || is_space || i == 0) {
                unsigned char parser_char = c;
                MarkInlineParse ips[2];
                size_t nips;

                if (is_space || (i == 0 && !is_punct)) parser_char = ' ';
                nips = mark_inline_parsers_for(parser_char, ips);
                if (nips != 0) {
                    long saved_line;
                    MarkSegment saved_position;
                    MarkNode *inline_node = NULL;
                    size_t k;

                    mark_reader_advance(block, n);
                    n = 0;
                    mark_reader_position(block, &saved_line, &saved_position);
                    if (i != 0) {
                        mark_reader_position(block, &current_l, &current_position);
                        if (!mark_merge_or_append_text(ctx->arena, parent,
                                                       mark_segment_between(start_position, current_position))) {
                            ctx->failed = 1;
                            return;
                        }
                        mark_reader_position(block, &current_l, &start_position);
                    }
                    for (k = 0; k < nips; k++) {
                        inline_node = ips[k](ctx, parent, block);
                        if (ctx->failed) return;
                        if (inline_node != NULL) break;
                        mark_reader_set_position(block, saved_line, saved_position);
                    }
                    if (inline_node != NULL) {
                        mark_node_append_child(parent, inline_node);
                        goto retry;
                    }
                }
            }
            if (escaped) {
                escaped = 0;
                n++;
                continue;
            }
            if (c == '\\') {
                escaped = 1;
                n++;
                continue;
            }
            escaped = 0;
            n++;
        }
        if (n != 0) mark_reader_advance(block, n);
        mark_reader_position(block, &current_l, &current_position);
        if (l != current_l) continue;
        diff = mark_segment_between(start_position, current_position);
        if ((line_break_flags & (LINE_BREAK_HARD | LINE_BREAK_VISIBLE)) == (LINE_BREAK_HARD | LINE_BREAK_VISIBLE)) {
            text = mark_text_new(ctx->arena, diff);
        } else {
            text = mark_text_new(ctx->arena, mark_segment_trim_right_space(diff, ctx->src, ctx->src_len));
        }
        if (text == NULL) {
            ctx->failed = 1;
            return;
        }
        if (line_break_flags & LINE_BREAK_SOFT) text->flags |= MARK_TEXT_SOFT_LINE_BREAK;
        if (line_break_flags & LINE_BREAK_HARD) text->flags |= MARK_TEXT_HARD_LINE_BREAK;
        mark_node_append_child(parent, text);
        mark_reader_advance_line(block);
    }

    mark_process_delimiters(ctx, NULL, 0);
    mark_link_close_block(ctx, parent, block);
}

/* parser.Parse: blocks, then every block's inlines in post-order
 * (parser.walkBlock, iterative here), with no AST transformers. */
MarkNode *mark_parse(MarkCtx *ctx)
{
    MarkReader reader;
    MarkReader block;
    MarkNode *root = mark_node_new(ctx->arena, MARK_DOCUMENT);
    MarkNode *n;

    if (root == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    mark_reader_init(&reader, ctx->src, ctx->src_len);
    parse_blocks(ctx, root, &reader);
    if (ctx->failed || ctx->limit) return NULL;

    mark_block_reader_init(&block, ctx->src, ctx->src_len, NULL);
    n = root;
    while (n->first != NULL) n = n->first;
    for (;;) {
        parse_block(ctx, &block, n);
        if (ctx->failed || ctx->limit) return NULL;
        if (n == root) break;
        if (n->next != NULL) {
            n = n->next;
            while (n->first != NULL) n = n->first;
        } else {
            n = n->parent;
        }
    }
    return root;
}
