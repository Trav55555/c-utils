/* libFuzzer harness for JSON. The first byte picks a mode.
 * Mode 0 parses the rest. Whatever parses must write back out without
 * failing, the written text must parse, and writing that again must give
 * the same bytes: the writer's output is a fixed point.
 * Otherwise base64: decoding the rest, when it succeeds, must re-encode to
 * exactly the input (one text per byte string), and encoding the rest must
 * decode back to it. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

static void emit(CuJsonOut *o, const CuJson *v)
{
    size_t i;

    switch (v->kind) {
    case CU_JSON_NULL:
        cu_json_null(o);
        break;
    case CU_JSON_BOOL:
        cu_json_bool(o, v->truth);
        break;
    case CU_JSON_NUMBER:
        cu_json_number(o, v->text);
        break;
    case CU_JSON_STRING:
        cu_json_string(o, v->text);
        break;
    case CU_JSON_ARRAY:
        cu_json_begin_array(o);
        for (i = 0; i < v->n; i++)
            emit(o, &v->items[i]);
        cu_json_end_array(o);
        break;
    case CU_JSON_OBJECT:
        cu_json_begin_object(o);
        for (i = 0; i < v->n; i++) {
            cu_json_key(o, v->keys[i]);
            emit(o, &v->items[i]);
        }
        cu_json_end_object(o);
        break;
    }
}

static CuStr write_out(CuArena *a, const CuJson *v)
{
    CuJsonOut o;
    CuStr out;

    cu_json_out_init(&o, a);
    emit(&o, v);
    if (!cu_json_out_finish(&o, &out))
        __builtin_trap();
    return out;
}

static void json(CuArena *a, CuStr in)
{
    CuJson v;
    CuJson again;
    CuJsonError err;
    CuStr first;
    CuStr second;

    if (!cu_json_parse(a, in, &v, &err)) {
        if (err.reason == NULL || err.offset > in.len)
            __builtin_trap();
        return;
    }
    first = write_out(a, &v);
    if (!cu_json_parse(a, first, &again, &err))
        __builtin_trap();
    second = write_out(a, &again);
    if (!cu_str_eq(first, second))
        __builtin_trap();
}

static void base64(CuArena *a, CuStr in)
{
    CuBuilder b;
    CuStr decoded;
    CuStr text;

    if (cu_base64_decode(a, in, &decoded)) {
        cu_sb_init(&b, a);
        cu_sb_base64(&b, decoded);
        if (!cu_sb_finish(&b, &text) || !cu_str_eq(text, in))
            __builtin_trap();
    }
    cu_sb_init(&b, a);
    cu_sb_base64(&b, in);
    if (!cu_sb_finish(&b, &text) || !cu_base64_decode(a, text, &decoded) ||
        !cu_str_eq(decoded, in))
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuStr in;

    if (size == 0 || size > 1 << 16)
        return 0;
    in = cu_str_n((const char *)data + 1, size - 1);
    cu_arena_init(&a);
    if (data[0] == 0)
        json(&a, in);
    else
        base64(&a, in);
    cu_arena_release(&a);
    cu_scratch_release();
    return 0;
}
