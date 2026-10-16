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

`cu_arena_set_limit(&a, max_bytes)` caps the bytes an arena's blocks may hold, so memory spent on untrusted input has a ceiling. A push past the cap fails exactly like running out of memory. After a refusal, `a.held + a.refused` is the smallest cap under which that push succeeds.

`cu_arena_fail_at(&a, n)` is the test hook: block allocation number `n` (counted in `a.attempts`) fails, and every other one succeeds. Each container's tests step `n` from 0 until a run makes no more than `n` allocations, so every out-of-memory path runs in ordinary tests. Failing one allocation, not every one past a cap, is what catches code that ignores a failure: later allocations still succeed, so nothing hides it.

## Temp scopes and containers

Containers (`CuBuilder`, `CuVec`, `CuMap`, `CuPool`, `CuPlex`) keep their memory in their arena and remember the temp scope that was innermost when they were initialised. Any mutating call (append, push, reserve, upsert, add) under another scope aborts, because ending that scope would free the container's memory under it. Reading is allowed under newer scopes while the container's own scope is open. To build something inside a scope, initialise the container inside it too. Growing a container after its arena is released aborts too. Re-initialising an arena that still has containers is not detected.

## Growth and speed

`CuVec` and `cu_stream_read` grow through `cu_arena_grow`: when the buffer is the arena's newest allocation it extends into the rest of its block, or reallocates a block it has to itself (made inside the innermost temp scope), so nothing is left behind. A buffer that something else was pushed after moves and leaves its old copy in the arena. A reallocation frees the old buffer, as `realloc` does; `CuVec` already invalidates element pointers on every push, and pushing one of its own elements is handled. Memory from before the innermost temp scope is never grown in place, so a scope's end still frees exactly what was pushed in it.

`CuBuilder` never reallocates: it grows in place within its block (`cu_arena_extend`) or moves to a new buffer and leaves the old bytes where they were. Views from `cu_sb_finish` therefore stay valid while the builder keeps growing, and a builder can append its own contents. The cost is that a builder past its block holds about twice its capacity, as before.

Ending a temp scope keeps its largest block for the arena's next block (except in debug mode), so a scope that is filled and ended in a loop does not go back to `malloc` each time. `a.cached` counts that block; `a.held` counts live blocks only, and the limit caps the two together.

`CU_STR_AT_FAST`, `CU_VEC_AT_FAST` and `CU_VEC_PUSH_FAST` do the common case in the header, after every check the function would make, and call the function otherwise. They evaluate their arguments more than once. Every other call crosses translation units, so build with `-flto` (or `-O2 -flto` like `dcheck`'s release check) to let the compiler inline the library into hot loops; there is no unity build file, because consumers compile `src/*.c` directly.

## Typed access

Containers and slices store values as bytes and hand them back as `void *`. Each typed macro (`CU_VEC_AT`, `CU_SLICE_AT`, `CU_MAP_GET`, `CU_POOL_GET`, `CU_PLEX_GET` and the rest) passes `sizeof` its type, and the call aborts if that differs from the size the container was initialised with. A type of a different size is therefore caught on every call, but a different type of the same size (`int` for `float`, or two structs that happen to match) is not: the size is the only type information that survives. `CuPlex` adds a kind number to each link, so links are checked by kind as well as size. Code held to MISRA-style rules should still treat each conversion from `void *` as a deviation to justify.

## Files and streams

`cu_file_read(&arena, path, &text)` reads a whole file, including NUL bytes,
and closes it on every path. `cu_stream_read(&arena, stream, &text)` reads
from the stream's current position to EOF and leaves it open. Both return
0 on I/O or allocation failure and set `text` to an empty, non-NULL view.
Allocation failures mark the arena; I/O failures leave its memory status
alone. The view lives until its arena scope ends.

The file size is an allocation hint. If it cannot be queried, the reader
rewinds and grows the buffer as needed. Streams also grow by doubling,
in place while the buffer is the arena's newest allocation; a buffer that
has to move leaves its old copy in the arena until the scope ends. Use the
arena byte limit when reading untrusted input.

`CuInput` provides chunked input without seeks or arena allocations. Open
an owned file with `cu_input_open`, or borrow an existing `FILE` at its
current position with `cu_input_attach`. The caller supplies each buffer.

```c
CuInput input;
CuInputStatus status;
char buffer[4096];
size_t n;

if (cu_input_open(&input, "data.txt")) {
    while ((status = cu_input_read(&input, buffer, sizeof buffer, &n)) == CU_INPUT_DATA) {
        /* Consume exactly n bytes; buffer is not NUL-terminated. */
    }
    if (!cu_input_close(&input)) {
        /* Inspect cu_input_errno(&input): first read or close error. */
    }
}
```

`CU_INPUT_END` and `CU_INPUT_ERROR` publish zero bytes. A failed read may
consume bytes and modify the buffer but publishes none. EOF and error
states are sticky. Closing preserves the first error, including errno
zero, and repeated closes do not call stdio again. A borrowed stream stays
open with its position and flags unchanged by detach. Keep its use
exclusive while attached, and close an active owner before reinitializing
or copying storage. Reads after successful close abort.

The first consumer is the streaming literal-search slice of
[cgrep](../cgrep/README.md).

## Directories

`CuDir` lists one directory: each entry once, in no particular order,
without `.` and `..`. Each entry has a name, copied into the arena with a
NUL after it, and a kind: file, directory, link (never followed), or other.
A walker keeps memory bounded by opening a temp scope per directory:

```c
CuTemp t;
CuDir dir;
CuDirEntry e;
int r;

t = cu_temp_begin(&arena);
if (cu_dir_open(&dir, &arena, path)) {
    while ((r = cu_dir_next(&dir, &e)) == 1) {
        /* Keep e.name; it lives until cu_temp_end(t). */
    }
    cu_dir_close(&dir);
    if (r < 0) {
        /* Inspect cu_dir_errno(&dir); 0 means memory ran out. */
    }
}
cu_temp_end(t);
```

Errors are sticky. Where the platform has no POSIX directories (Cerberus,
for one), `cu_dir_open` fails with `ENOSYS`. Builds that include a system
header before `src/cu_dir.c` need `_DEFAULT_SOURCE` defined, as
`dcheck.conf` does.

## Checks

```sh
make test    # plain build and run of every test; needs only a C compiler
make check   # dcheck gate: C89 subset, lint, gcc/clang/tcc matrix, analyzer,
             # ASan+UBSan, MSan, -O2 -flto, wasm32-wasi, coverage >= 90%
make deep    # + Cerberus, valgrind, libFuzzer, CBMC proofs
```

`dcheck` lives in `~/projects/dependable-c-agent-toolchain`; override with `make DCHECK=/path/to/dcheck`.

Proofs: `proofs/proof_str.c` proves slicing, find and cut for every string of up to 4 bytes; `proofs/proof_size.c` proves `cu_size_add` for all inputs. `cu_size_mul` is fuzzed against `__builtin_mul_overflow` (`fuzz/fuzz_size.c`) because a 64-bit multiply proof does not finish in CBMC.

## License

MIT. See `LICENSE`.
