#ifndef MARK_PARSER_H
#define MARK_PARSER_H
#include "cu_mark.h"
#include "mark_ast.h"

/* goldmark's parser package and the parsers of its extension package.
 * mark_parser.c is parser/parser.go (the context, the block and inline
 * drivers, auto ids, and Hugo's id factory); mark_block.c holds the block
 * parsers, parser/link_ref.go and parser/attribute.go; mark_inline.c holds
 * the inline parsers and parser/delimiter.go; mark_ext.c holds the block
 * parsers and transformers of extension/table.go, definition_list.go and
 * footnote.go; mark_ext_inline.c the inline parsers of strikethrough.go,
 * tasklist.go, linkify.go, typographer.go and footnote.go.
 *
 * Every parse function takes the MarkCtx. Allocation failures set
 * ctx->failed (the arena is marked too) and the drivers stop at the next
 * check; a function that got a NULL node returns as if nothing parsed.
 * A passed bound sets ctx->limit. cu_mark_render maps both to a status. */

/* parser.Reference */
typedef struct MarkReference {
    CuStr label;
    CuStr destination;
    CuStr title;
    int has_title; /* 0: Go's nil title */
} MarkReference;

/* parser.State bits */
#define MARK_STATE_NONE 1u
#define MARK_STATE_CONTINUE 2u
#define MARK_STATE_CLOSE 4u
#define MARK_STATE_HAS_CHILDREN 8u
#define MARK_STATE_NO_CHILDREN 16u
#define MARK_STATE_REQUIRE_PARAGRAPH 32u

/* The block parsers in priority order: parser.DefaultBlockParsers (100,
 * 200, ... 1000) with the extensions' parsers at their priorities
 * (definition list 101 and 102, footnote 999). */
typedef enum MarkBlockParserId {
    MARK_BP_SETEXT_HEADING,
    MARK_BP_DEFINITION_LIST,
    MARK_BP_DEFINITION_DESCRIPTION,
    MARK_BP_THEMATIC_BREAK,
    MARK_BP_LIST,
    MARK_BP_LIST_ITEM,
    MARK_BP_CODE_BLOCK,
    MARK_BP_ATX_HEADING,
    MARK_BP_FENCED_CODE_BLOCK,
    MARK_BP_BLOCKQUOTE,
    MARK_BP_HTML_BLOCK,
    MARK_BP_FOOTNOTE,
    MARK_BP_PARAGRAPH,
    MARK_BP_COUNT
} MarkBlockParserId;

/* parser.Block: an open block and the parser that owns it. */
typedef struct MarkBlock {
    MarkNode *node;
    MarkBlockParserId parser;
} MarkBlock;

typedef struct MarkCtx {
    CuArena *arena;
    const unsigned char *src;
    long src_len;
    unsigned long flags; /* CU_MARK_* option bits */
    int failed;
    int limit;
    /* parseContext */
    CuMap refs;          /* MarkReference by util.ToLinkReference(label) */
    CuMap ids;           /* ids.values: int by id */
    long block_offset;
    long block_indent;
    MarkNode *delimiters;
    MarkNode *last_delimiter;
    CuVec opened;        /* MarkBlock */
    /* context keys of the block parsers */
    MarkNode *temporary_paragraph;        /* setext_headings.go */
    int skip_list_parser;                 /* list.go */
    int empty_list_item_with_blank_lines; /* list.go */
    int fence_set;                        /* fcode_block.go fenceData */
    unsigned char fence_char;
    long fence_indent;
    long fence_length;
    MarkNode *fence_node;
    /* context keys of the link parser */
    MarkNode *link_label_state; /* linkLabelStateKey: the list's head */
    MarkNode *link_bottom;      /* linkBottom; NULL with link_bottom_set is
                                 * Go's typed nil *Delimiter in an interface */
    int link_bottom_set;
    /* context keys of the extensions */
    long typo_single;          /* typographer.go unclosedCounter: never reset
                                * (its CloseBlock is not a CloseBlocker) */
    long typo_double;
    CuVec escaped_pipe_cells;  /* table.go escapedPipeCellListKey: MarkEscapedCell* */
    MarkNode *footnote_list;   /* footnote.go footnoteListKey */
    CuVec footnote_links;      /* footnoteLinkListKey: MarkNode* */
} MarkCtx;

/* table.go escapedPipeCell */
typedef struct MarkEscapedCell {
    MarkNode *cell;
    CuVec pos; /* long */
    int transformed;
} MarkEscapedCell;

/* parser.BlockParser as a table entry. open returns the node or NULL with
 * *state set; cont returns the state. */
typedef struct MarkBlockParser {
    MarkNode *(*open)(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state);
    unsigned (*cont)(MarkCtx *ctx, MarkNode *node, MarkReader *r);
    void (*close)(MarkCtx *ctx, MarkNode *node, MarkReader *r);
    int can_interrupt_paragraph;
    int can_accept_indented_line;
    const char *trigger; /* bytes, or NULL for a free parser */
    unsigned long flag;  /* the CU_MARK_* option that registers it; 0: core */
} MarkBlockParser;

extern const MarkBlockParser mark_block_parsers[MARK_BP_COUNT];

/* ---- mark_parser.c ---- */

/* parser.NewContext; the containers live in ctx->arena. */
void mark_ctx_init(MarkCtx *ctx, CuArena *a, CuStr src, unsigned long flags);
/* Reads (parser.Parse) the whole source into a Document, or NULL when
 * ctx->failed or ctx->limit is set. */
CU_MUST_USE MarkNode *mark_parse(MarkCtx *ctx);
/* Context.LastOpenedBlock; node NULL when none is open */
MarkBlock mark_last_opened_block(const MarkCtx *ctx);
/* Context.AddReference / Reference */
void mark_add_reference(MarkCtx *ctx, MarkReference ref);
const MarkReference *mark_reference(MarkCtx *ctx, CuStr key);
/* IDs.Generate for a heading (Hugo's idFactory under CU_MARK_HUGO_IDS),
 * and IDs.Put */
CU_MUST_USE CuStr mark_ids_generate(MarkCtx *ctx, CuStr value);
void mark_ids_put(MarkCtx *ctx, CuStr id);
/* Appends to a line vector, recording a failure in ctx. */
void mark_lines_append(MarkCtx *ctx, CuVec *lines, MarkSegment s);
/* Segments.At with Go's invalid segment for an index out of range. */
MarkSegment mark_lines_at(const CuVec *lines, long i);
/* Segments.SetSliced(0, n) */
void mark_lines_truncate(CuVec *lines, size_t n);

/* ---- mark_block.c ---- */

/* LinkReferenceParagraphTransformer.Transform */
void mark_link_reference_transform(MarkCtx *ctx, MarkNode *node, MarkReader *r);

/* ---- mark_inline.c ---- */

/* The inline parsers registered for trigger byte c under ctx->flags, in
 * priority order (parser.DefaultInlineParsers at 100..500, task list 0,
 * footnote 101, strikethrough 500, linkify 999, typographer 9999); returns
 * how many were stored in out. */
#define MARK_INLINE_PARSERS_MAX 4
typedef MarkNode *(*MarkInlineParse)(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
size_t mark_inline_parsers_for(const MarkCtx *ctx, unsigned char c, MarkInlineParse out[MARK_INLINE_PARSERS_MAX]);
/* parser.ProcessDelimiters; bottom_set distinguishes Go's nil interface
 * from a stored nil *Delimiter. */
void mark_process_delimiters(MarkCtx *ctx, MarkNode *bottom, int bottom_set);
/* ScanDelimiter's flags for the run of line[0] at the start of line, as
 * the emphasis, strikethrough and typographer processors compute them;
 * returns the run length. */
long mark_scan_delimiter_flags(MarkLine line, unsigned long before, int *can_open, int *can_close);
/* parseContext.PushDelimiter */
void mark_push_delimiter(MarkCtx *ctx, MarkNode *d);
/* linkParser.CloseBlock */
void mark_link_close_block(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
/* parseLinkDestination (link.go), shared with link_ref.go */
CU_MUST_USE int mark_parse_link_destination(MarkCtx *ctx, MarkReader *block, CuStr *out);

/* ---- mark_ext.c ---- */

/* tableParagraphTransformer.Transform */
void mark_table_transform(MarkCtx *ctx, MarkNode *node, MarkReader *r);
/* tableASTTransformer.Transform and footnoteASTTransformer.Transform */
void mark_table_ast_transform(MarkCtx *ctx, MarkNode *doc);
void mark_footnote_ast_transform(MarkCtx *ctx, MarkNode *doc);
/* The block parsers of definition_list.go and footnote.go */
MarkNode *mark_definition_list_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state);
unsigned mark_definition_list_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r);
MarkNode *mark_definition_description_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state);
unsigned mark_definition_description_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r);
void mark_definition_description_close(MarkCtx *ctx, MarkNode *node, MarkReader *r);
MarkNode *mark_footnote_block_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state);
unsigned mark_footnote_block_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r);
void mark_footnote_block_close(MarkCtx *ctx, MarkNode *node, MarkReader *r);

/* ---- mark_ext_inline.c ---- */

MarkNode *mark_strikethrough_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
MarkNode *mark_task_checkbox_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
MarkNode *mark_linkify_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
MarkNode *mark_typographer_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block);
MarkNode *mark_footnote_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block);

#endif
