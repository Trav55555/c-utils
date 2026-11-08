#ifndef CU_MARK_H
#define CU_MARK_H
#include "cu.h"

/* c-mark: Markdown to HTML, a Dependable C port of goldmark v1.6.0
 * (github.com/yuin/goldmark, MIT, Copyright (c) 2019 Yusuke Inuzuka).
 *
 * For every input and every supported option set, the output is the byte
 * string goldmark v1.6.0 renders with the same configuration. The oracle
 * (c-mark/oracle) defines that string; the corpus and differential tests
 * check it. Where goldmark has a bug, c-mark has the same bug: the point is
 * to replace Hugo's renderer without changing a published page.
 *
 * CU_MARK_HUGO is the configuration Hugo 0.120.4 uses by default (its
 * markup/goldmark/convert.go with goldmark_config.Default), including Hugo's
 * own heading ids.
 *
 * Not ported: CJK options, emoji, custom linkify regexps, footnote option
 * strings and id functions, custom typographer substitutions, render hooks
 * other than fenced code, and goldmark's plugin interfaces. */

/* Extensions and parser options. */
#define CU_MARK_TABLE           0x0001UL
#define CU_MARK_STRIKETHROUGH   0x0002UL
#define CU_MARK_LINKIFY         0x0004UL
#define CU_MARK_TASKLIST        0x0008UL
#define CU_MARK_TYPOGRAPHER     0x0010UL
#define CU_MARK_DEFINITION_LIST 0x0020UL
#define CU_MARK_FOOTNOTE        0x0040UL
#define CU_MARK_AUTO_HEADING_ID 0x0080UL  /* parser.WithAutoHeadingID */
#define CU_MARK_ATTRIBUTE       0x0100UL  /* parser.WithAttribute */
#define CU_MARK_HUGO_IDS        0x0200UL  /* Hugo's "github" id factory, not goldmark's */
/* Renderer options. */
#define CU_MARK_UNSAFE          0x1000UL  /* html.WithUnsafe: raw HTML and dangerous URLs pass */
#define CU_MARK_XHTML           0x2000UL  /* html.WithXHTML */
#define CU_MARK_HARD_WRAPS      0x4000UL  /* html.WithHardWraps */

#define CU_MARK_HUGO (CU_MARK_TABLE | CU_MARK_STRIKETHROUGH | CU_MARK_LINKIFY | CU_MARK_TASKLIST | \
                      CU_MARK_TYPOGRAPHER | CU_MARK_DEFINITION_LIST | CU_MARK_FOOTNOTE | \
                      CU_MARK_AUTO_HEADING_ID | CU_MARK_ATTRIBUTE | CU_MARK_HUGO_IDS)

/* extension.WithTableCellAlignMethod. DEFAULT is goldmark's default: the
 * align attribute under XHTML, a style attribute otherwise. */
typedef enum CuMarkTableAlign {
    CU_MARK_ALIGN_DEFAULT = 0,
    CU_MARK_ALIGN_ATTRIBUTE,
    CU_MARK_ALIGN_STYLE,
    CU_MARK_ALIGN_NONE
} CuMarkTableAlign;

/* One fenced code block, borrowed for the duration of the hook call.
 * info is the whole info string (empty when there is none); lang is
 * goldmark's Language(): info up to the first space or '{'; text is the
 * content lines, each with its newline, exactly as goldmark stores them.
 * attrs holds the block's {...} attributes as name/value pairs in source
 * order (values unquoted, entities not decoded), count in attr_count. */
typedef struct CuMarkAttr {
    CuStr name;
    CuStr value;
} CuMarkAttr;

typedef struct CuMarkCode {
    CuStr info;
    CuStr lang;
    CuStr text;
    const CuMarkAttr *attrs;
    size_t attr_count;
} CuMarkCode;

/* Renders a fenced code block in place of goldmark's default. Appends to
 * out and returns 1, or returns 0 to keep the default rendering. Errors go
 * through out (a failed builder fails the render). */
typedef int (*CuMarkCodeHook)(void *ctx, const CuMarkCode *code, CuBuilder *out);

typedef struct CuMarkOptions {
    unsigned long flags;
    CuMarkTableAlign table_align;
    CuMarkCodeHook code_hook;  /* NULL: goldmark's rendering */
    void *code_ctx;
} CuMarkOptions;

typedef enum CuMarkStatus {
    CU_MARK_OK = 0,
    CU_MARK_NOMEM,  /* an arena allocation failed */
    CU_MARK_LIMIT   /* the input passes a bound below; nothing was rendered */
} CuMarkStatus;

/* Bounds. goldmark has none; a dependable renderer needs them. An input
 * inside them renders exactly as goldmark renders it; one outside them is
 * refused whole (CU_MARK_LIMIT), never rendered differently. */
#define CU_MARK_MAX_INPUT  (16UL << 20)  /* bytes */
#define CU_MARK_MAX_DEPTH  256           /* nested block containers */

/* Renders src (any bytes; invalid UTF-8 is handled as goldmark handles it)
 * and appends the HTML to out. scratch holds the document tree and is used
 * under a temp scope that is ended before returning, so it may be the
 * builder's arena only if the builder was initialised outside that scope;
 * pass a separate arena when in doubt. On a status other than CU_MARK_OK,
 * out may hold a partial document and should be discarded. */
CU_MUST_USE CuMarkStatus cu_mark_render(CuArena *scratch, CuStr src, const CuMarkOptions *opt, CuBuilder *out);

#endif
