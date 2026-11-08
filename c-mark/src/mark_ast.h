#ifndef MARK_AST_H
#define MARK_AST_H
#include "mark_text.h"

/* goldmark's ast package (ast/ast.go, block.go, inline.go), the extension
 * nodes of extension/ast, plus the two parser-private inline node types
 * (parser.Delimiter, parser.linkLabelState) that live in the tree while a
 * block's inlines are parsed. One struct holds
 * every node type: the fields a kind does not use stay at their zero
 * values. Nodes are arena memory and are never freed individually; a node
 * detached from the tree stays valid, as goldmark's garbage-collected nodes
 * do, and pointer identity is stable, which the delimiter code relies on. */

typedef enum MarkKind {
    MARK_DOCUMENT,
    MARK_TEXT_BLOCK,
    MARK_PARAGRAPH,
    MARK_HEADING,
    MARK_THEMATIC_BREAK,
    MARK_CODE_BLOCK,
    MARK_FENCED_CODE_BLOCK,
    MARK_BLOCKQUOTE,
    MARK_LIST,
    MARK_LIST_ITEM,
    MARK_HTML_BLOCK,
    MARK_TEXT,
    MARK_STRING,
    MARK_CODE_SPAN,
    MARK_EMPHASIS,
    MARK_LINK,
    MARK_IMAGE,
    MARK_AUTO_LINK,
    MARK_RAW_HTML,
    MARK_DELIMITER,
    MARK_LINK_LABEL_STATE,
    /* extension/ast */
    MARK_TABLE,
    MARK_TABLE_HEADER,
    MARK_TABLE_ROW,
    MARK_TABLE_CELL,
    MARK_STRIKETHROUGH,
    MARK_TASK_CHECKBOX,
    MARK_DEFINITION_LIST,
    MARK_DEFINITION_TERM,
    MARK_DEFINITION_DESCRIPTION,
    MARK_FOOTNOTE_LINK,
    MARK_FOOTNOTE_BACKLINK,
    MARK_FOOTNOTE,
    MARK_FOOTNOTE_LIST
} MarkKind;

/* extension/ast.Alignment */
#define MARK_ALIGN_LEFT 1
#define MARK_ALIGN_RIGHT 2
#define MARK_ALIGN_CENTER 3
#define MARK_ALIGN_NONE 4

/* ast.Attribute. goldmark stores values as interface{}; the parser makes
 * strings, booleans, null, numbers, arrays and nested attribute maps, and
 * only strings are ever rendered (the renderer's type assertion panics on
 * anything else, see mark_html.c), so the other kinds keep no payload. */
typedef enum MarkAttrKind {
    MARK_ATTR_STRING,
    MARK_ATTR_BOOL,
    MARK_ATTR_NULL,
    MARK_ATTR_NUMBER,
    MARK_ATTR_ARRAY,
    MARK_ATTR_MAP
} MarkAttrKind;

typedef struct MarkAttrValue {
    MarkAttrKind kind;
    CuStr str; /* STRING: the bytes */
} MarkAttrValue;

typedef struct MarkAttr {
    CuStr name;
    MarkAttrValue value;
} MarkAttr;

/* ast.Text flags */
#define MARK_TEXT_SOFT_LINE_BREAK 1u
#define MARK_TEXT_HARD_LINE_BREAK 2u
#define MARK_TEXT_RAW 4u
#define MARK_TEXT_CODE 8u

/* ast.AutoLinkType */
#define MARK_AUTOLINK_EMAIL 1
#define MARK_AUTOLINK_URL 2

typedef struct MarkNode MarkNode;
struct MarkNode {
    MarkKind kind;
    MarkNode *parent;
    MarkNode *first;
    MarkNode *last;
    MarkNode *prev;
    MarkNode *next;
    long child_count;
    CuVec attrs;            /* MarkAttr; Go's nil slice is len 0 */
    /* BaseBlock */
    int blank_previous_lines;
    CuVec *lines;           /* MarkSegment; NULL until Lines() is asked for */
    /* Heading.Level, Emphasis.Level */
    long level;
    /* List */
    unsigned char marker;
    int is_tight;
    long start;
    /* ListItem.Offset */
    long offset;
    /* HTMLBlock */
    int html_block_type;
    MarkSegment closure_line;
    /* FencedCodeBlock.Info, and its cached Language() */
    MarkNode *info;
    int has_language;
    CuStr language;
    /* Text.Segment, Delimiter.Segment, linkLabelState.Segment */
    MarkSegment segment;
    unsigned flags;         /* Text and String flags */
    /* String.Value */
    CuStr value;
    /* baseLink; has_title tells Go's nil Title from an empty one */
    CuStr destination;
    CuStr title;
    int has_title;
    /* AutoLink; protocol is Go's nil when empty */
    int autolink_type;
    MarkNode *autolink_value;
    CuStr protocol;
    /* RawHTML.Segments */
    CuVec *segments;
    /* Delimiter */
    int can_open;
    int can_close;
    long length;
    long original_length;
    unsigned char delim_char;
    MarkNode *prev_delim;
    MarkNode *next_delim;
    /* linkLabelState */
    int is_image;
    MarkNode *ll_prev;
    MarkNode *ll_next;
    MarkNode *ll_first;
    MarkNode *ll_last;
    /* Table.Alignments (int), TableCell.Alignment */
    CuVec *alignments;
    int alignment;
    /* TaskCheckBox.IsChecked */
    int is_checked;
    /* DefinitionList: Offset is offset above, TemporaryParagraph here;
     * DefinitionDescription.IsTight is is_tight above */
    MarkNode *temporary_paragraph;
    /* Footnote.Ref and Index (-1 until referenced); FootnoteLink and
     * FootnoteBacklink Index, RefCount, RefIndex; FootnoteList.Count */
    CuStr ref;
    long index;
    long ref_count;
    long ref_index;
    long count;
};

/* A new node of kind with every field initialised; NULL on an arena
 * failure (the arena is then marked). */
CU_MUST_USE MarkNode *mark_node_new(CuArena *a, MarkKind kind);
/* A new CuVec of MarkSegment in a, or NULL. */
CU_MUST_USE CuVec *mark_segments_new(CuArena *a);
/* Node.Lines(): the node's lines, made on first use; NULL on failure. */
CU_MUST_USE CuVec *mark_node_lines(CuArena *a, MarkNode *n);
/* Node.IsRaw */
int mark_node_is_raw(const MarkNode *n);

/* BaseNode child operations */
void mark_node_append_child(MarkNode *n, MarkNode *child);
void mark_node_remove_child(MarkNode *n, MarkNode *child);
void mark_node_insert_before(MarkNode *n, MarkNode *v1, MarkNode *insertee);
void mark_node_insert_after(MarkNode *n, MarkNode *v1, MarkNode *insertee);
void mark_node_replace_child(MarkNode *n, MarkNode *v1, MarkNode *insertee);

/* BaseNode.SetAttribute / Attribute. The attribute vector grows in a. */
void mark_node_set_attribute(MarkNode *n, CuStr name, MarkAttrValue value);
const MarkAttr *mark_node_attribute(const MarkNode *n, CuStr name);
/* Node.Attributes() != nil */
int mark_node_has_attributes(const MarkNode *n);

MarkAttrValue mark_attr_string(CuStr s);
MarkAttrValue mark_attr_other(MarkAttrKind kind);

/* ast.NewTextSegment, NewRawTextSegment */
CU_MUST_USE MarkNode *mark_text_new(CuArena *a, MarkSegment s);
CU_MUST_USE MarkNode *mark_raw_text_new(CuArena *a, MarkSegment s);
/* ast.MergeOrAppendTextSegment, MergeOrReplaceTextSegment; 0 on failure */
CU_MUST_USE int mark_merge_or_append_text(CuArena *a, MarkNode *parent, MarkSegment s);
CU_MUST_USE int mark_merge_or_replace_text(CuArena *a, MarkNode *parent, MarkNode *n, MarkSegment s);
/* ast.NewImage(link): takes the link's children, destination and title */
CU_MUST_USE MarkNode *mark_image_new(CuArena *a, MarkNode *link);
/* List.IsOrdered */
int mark_list_is_ordered(const MarkNode *list);
/* FencedCodeBlock.Language; *has is 0 for Go's nil */
CuStr mark_fenced_language(MarkNode *n, const unsigned char *src, long len, int *has);
/* The depth of n: 0 for a node without a parent. */
long mark_node_depth(const MarkNode *n);
/* BaseNode.SortChildren with the footnote comparator (by index) */
void mark_node_sort_children_by_index(MarkNode *n);
/* AutoLink.URL: protocol + "://" + value when there is a protocol. */
CU_MUST_USE CuStr mark_autolink_url(CuArena *a, const MarkNode *n, const unsigned char *src, long len);

#endif
