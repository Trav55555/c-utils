# c-utils

Safe, fast primitives for [Dependable C](https://dependablec.org/) (the C89 subset that `dcheck` enforces). Each primitive removes a class of bugs by construction rather than by care.

| Bug class | Primitive |
|---|---|
| Use after free, double free, leaks | `CuArena` with temp scopes and per-thread scratch arenas: no per-object `free`; a scope ended out of order or twice aborts, and so does growing a container under a scope that would free its memory |
| Overflow in `count * size` | `cu_arena_push(a, count, elem_size)` checks the multiply; `cu_size_add` and `cu_size_mul` for your own sizes |
| Buffer overruns on strings | `CuStr` views carry their length; slicing clamps; `cu_str_at` aborts out of bounds; `CU_LIT` only accepts literals |
| `NULL` + length bugs | `CuStr.ptr` is never `NULL` |
| `sprintf` overflows and format mismatches | `CuBuilder`: typed appends, no format strings |
| `atoi`/`strtol` silent failures | `cu_parse_ulong`/`cu_parse_long`: whole-string, overflow-checked, ok flag |
| `<ctype.h>` UB on negative `char` and locale surprises | `cu_ascii_*` |
| Hash tables that rehash, move entries or borrow keys | `CuMap` hash trie: arena-backed, entries never move, keys copied, iteration in insertion order |
| Dangling pointers to long-lived objects | `CuPool`: generational handles; a stale, null or forged handle gets `NULL`, never memory |
| Dangling and mistyped links in graphs, cycles and back links | `CuPlex`: nodes link by typed `CuLink`, so a removed or wrong-kind node is `NULL`, never followed |
| Pointer casts, misaligned loads and sign extension in binary formats | `cu_rd_u32le` and friends read bounds-checked, endian-explicit integers from a `CuStr`; `cu_sb_u32le` and friends write them |
| Array overruns and element-type mix-ups | `CuVec` (arena-backed growth) and `CuSlice` (views with clamped sub-slicing): bounds-checked `AT`, element size checked at every call |
| Ignored errors | `CU_MUST_USE` on every fallible call; failures are sticky so one check covers a sequence |

Error policy, memory policy and every function's contract are at the top of `include/cu.h`.

## Example

```c
#include "cu.h"

/* Returns "name=value" built in the caller's arena. */
CuStr pair(CuArena *out, CuStr name, unsigned long value)
{
    CuBuilder b;
    CuStr s;

    cu_sb_init(&b, out);
    cu_sb_str(&b, name);
    cu_sb_char(&b, '=');
    cu_sb_ulong(&b, value);
    if (!cu_sb_finish(&b, &s))
        return cu_str_empty();
    return s;
}
```

## Debug arenas

With `CU_ARENA_DEBUG` defined, or when built with AddressSanitizer, every push is its own `malloc`. Sanitizers (and Fil-C) then see each object's bounds, and memory from an ended temp scope is really freed, so a stale pointer is reported instead of silently reading reused memory. `cu_arena_init_with(&a, block_size, 1)` turns it on for one arena.

## Arena limits

`cu_arena_set_limit(&a, max_bytes)` caps the bytes an arena's blocks may hold. A push past the cap fails exactly like running out of memory, so every container's out-of-memory path runs in ordinary tests, and memory spent on untrusted input has a ceiling. After a refusal, `a.held + a.refused` is the smallest cap under which that push succeeds; the tests use it to make each allocation in a scenario fail in turn.

## Temp scopes and containers

Containers (`CuBuilder`, `CuVec`, `CuMap`, `CuPool`, `CuPlex`) keep their memory in their arena and remember the temp scope that was innermost when they were initialised. Any mutating call (append, push, reserve, upsert, add) under another scope aborts, because ending that scope would free the container's memory under it. Reading is allowed under newer scopes while the container's own scope is open. To build something inside a scope, initialise the container inside it too. Growing a container after its arena is released aborts too. Re-initialising an arena that still has containers is not detected.

## Checks

```sh
make test    # plain build and run of every test; needs only a C compiler
make check   # dcheck gate: C89 subset, lint, gcc/clang/tcc matrix, analyzer,
             # ASan+UBSan, MSan, -O2 -flto, wasm32-wasi, coverage >= 90%
make deep    # + Cerberus, valgrind, libFuzzer, CBMC proofs
```

`dcheck` lives in `~/projects/dependable-c-agent-toolchain`; override with `make DCHECK=/path/to/dcheck`.

Proofs: `proofs/proof_str.c` proves slicing, find and cut for every string of up to 4 bytes; `proofs/proof_size.c` proves `cu_size_add` for all inputs. `cu_size_mul` is fuzzed against `__builtin_mul_overflow` (`fuzz/fuzz_size.c`) because a 64-bit multiply proof does not finish in CBMC.
