# Performance review, 2026-10-14

A critical review of c-utils as a general-purpose foundation. Timings are
indicative: Ryzen 5 7600, GCC 15.2, `-std=c89 -O2`, separate translation
units as consumers build them, load average about 4, best of 5. Prototype
fixes passed `make test`.

## Findings

1. **Two `div` per arena push.** `push_aligned` computed
   `(align - used % align) % align` with a runtime `align`; GCC kept both
   divisions. `cu_arena_push(a, 1, 16)` took 5.3 ns versus 0.45 ns for a
   mask-aligned bump and 9.0 ns for `malloc(16)`. Alignments are powers of
   two, so `(0 - used) & (align - 1)` is equivalent: 2.3 ns.
2. **Growth abandons old buffers.** `CuVec`, `CuBuilder` and
   `cu_stream_read` double into a fresh push and leave the old buffer in
   the arena. Held memory was 2.10x payload for a vector of 4M longs,
   3.36x for a 20 MB builder and 3.20x for a 10 MiB stream read. Every
   copy also lands on unfaulted memory. Growing in place when the buffer
   is the arena's newest allocation removes both costs.
3. **Out-of-line calls per element.** A pre-reserved `CU_VEC_PUSH` cost
   9.3 ns (two cross-unit scope checks and a runtime-size `memcpy`),
   `cu_sb_char` 6.1 ns, `cu_str_at` 1.36 ns versus 0.13 ns for indexing.
   Reading the scope field directly and calling `reserve` only when full
   gave 4.3 ns; header fast paths gave about 1 ns. A unity build (or LTO)
   cut `CU_VEC_AT` from 2.5 to 0.28 ns.
4. **CuMap.** Three pushes and three cache lines per entry (95.4 B for a
   9-byte key and 8-byte value), and a full key comparison at every trie
   level. Random-order hits took 320 ns versus 136 ns for a plain
   open-addressing table. Storing the hash in each node cut hits from 58
   to 48 ns sequential and 320 to 234 ns random, and misses from 21 to
   13 ns; a single push per entry offsets the larger node.
5. **Endian readers.** `le()`/`be()` take the width at runtime; GCC keeps
   a byte loop (2.5 ns per u32 versus 0.12 ns for a `memcpy` load).
   Width-specific expressions compile to one load.
6. **`cu_parse_ulong`.** One division and up to three cross-unit calls per
   digit: 19.0 ns for 10 digits, the same as `strtoul`. Hoisting the
   overflow limit and classifying digits locally: 7.9 ns.
7. **Integer formatting.** `to_digits` divides by a runtime base per
   digit: `cu_sb_ulong` 26 ns, only 20% faster than `snprintf`. Base-10
   and base-16 specializations remove the divisions (22.9 and 11.6 ns);
   a digit-pair table halves the dependent chain.
8. **CuPool.** Slot headers and values live in separate pushes, so a get
   makes three dependent loads. Random gets took 94 ns versus 15 ns for a
   raw random dereference; held memory was 1.80x payload.
9. **Arena blocks.** Fixed 64 KiB blocks with no reuse at scope end. A
   64 MiB scope cost 27.4 ms and 16,385 minor faults per iteration.
   Alternating 100 B and 70,000 B pushes held 1.93x the requested bytes.

## Secondary

- `cu_str_find` is `memchr` plus `memcmp` per candidate: O(n*m) on
  adversarial input (6.7 vs 2.4 ns/byte against `memmem` for `a^256 b`).
- `cu_sb_long` reserves twice; `cu_sb_char` has no in-capacity fast path.
- `find()` in `cu_map.c` is not inlined because `root` is address-taken.
- `cu_input_read` runs at `read(2)` speed only for chunks of at least
  64 KiB (3.9 vs 4.4 GB/s at 4 KiB).

## Looked fine

The overflow-checked multiply (`mul; jo`), abort paths (already moved to
`.text.unlikely`), no zeroing, per-thread scratch arenas (9-11 ns per
empty scope), `CuStr` by value (two registers), FNV-1a for short keys, and
the sized `cu_file_read` path (one push, 1.00x).

The uncommitted `src/cu_arena.c` edit present at review time was
formatting only: object code was identical at `-O2`.

## Disposition

Before/after: the scratch microbenchmarks built at `-std=c89 -O2` with
separate translation units, the snapshot taken before these changes and the
new sources run back to back on 2026-10-15 (load average about 5). Best of
5; indicative only.

| # | Finding | Status | What changed, or why not | Before | After |
|---|---|---|---|---|---|
| 1 | Two `div` per arena push | Done | `(0 - used) & (align - 1)`. `CU_ALIGN` is now the largest power of two dividing the max-align union's size, so it stays a power of two where that size is not (12 on i386). | push 5.80 ns, push_bytes 4.76 ns | 2.58 ns, 1.62 ns |
| 2 | Growth abandons old buffers | Done for `CuVec` and `cu_stream_read`; for `CuBuilder` on 2026-10-15: it reallocates through `cu_arena_grow` until `cu_sb_finish` hands out a view, then extends in place or copies so the view stays valid (tests/test_fmt.c: a builder of 2^20 + 1 bytes now holds under twice its length plus a block). The 20 MB builder, finished once, went from 3.35x to 1.68x: doubling's spare capacity, nothing abandoned. | New `cu_arena_grow`: in place when the buffer is the newest allocation and fits its block; `realloc` of the head block when the buffer is alone in it and the block was made inside the innermost temp scope; else push and copy. A zero-size old allocation always gets a new push. Never in place over memory from before the innermost scope (the arena tracks that scope's start). Used by `CuVec` (which copies or re-finds an element pushed from its own buffer) and `cu_stream_read`. `CuBuilder` uses `cu_arena_extend` (in place only) and otherwise pushes and copies, because views from `cu_sb_finish` must outlive later appends; a reallocating builder freed them. Accounting in `cu.h`. | vec 4M longs 2.10x, builder 20 MB 3.36x, stream 10 MiB 3.20x / 11.0 ms; `CU_VEC_PUSH` 13.5 ns | 1.05x, 3.35x, 1.60x / 4.9 ms; 6.5 ns |
| 3 | Out-of-line calls per element | Done, unity build not added | Scope checks read the arena field; `CU_VEC_PUSH` reserves only when full; `cu_sb_char` has an in-capacity path; opt-in `CU_STR_AT_FAST`, `CU_VEC_AT_FAST`, `CU_VEC_PUSH_FAST` make every check inline and call the function otherwise (they evaluate arguments more than once). No unity file: consumers compile `src/*.c`; README recommends `-flto`. | pre-reserved push 8.16 ns, `cu_sb_char` 4.79 ns, `cu_str_at` 1.00 ns | 3.93 ns (macro 0.86 ns), 1.75 ns, `CU_STR_AT_FAST` 0.09 ns |
| 4 | CuMap | Done | Hash stored in each node and compared before key bytes; one push per entry (node, value at a max-aligned offset, key bytes); `get` walks nodes, not links. Held per entry unchanged: one 81-byte push rounds to 96, as the three pushes did. | insert 49.0 ns; hit seq 35.6, random 147.6, miss 16.0 ns; 95.4 B/entry | 31.6 ns; 23.2, 88.5, 9.7 ns; 95.4 B/entry |
| 5 | Endian readers | Done | One width-specific expression per reader. The remaining cost is the call. | `cu_rd_u32le` 2.08 ns | 1.18 ns |
| 6 | `cu_parse_ulong` | Done | Overflow limit and remainder computed once; digits classified locally. | 18.3 ns (strtoul 14.2) | 7.6 ns |
| 7 | Integer formatting | Done | Decimal by a two-digit table, hex by shifts; `cu_sb_long` writes sign and digits in one append. | `cu_sb_ulong` 20.7 ns, `cu_sb_hex` 13.7 ns (snprintf 24.1) | 11.0 ns, 8.9 ns |
| 8 | CuPool | Done | Header and value share a slot, one push per 64-slot page; `live` folded into `next_free` (a `CU_LIVE` marker); page found once per get. Index still checked before any slot is read. | add 19.2 ns; get seq 6.75, random 54.0 ns; 1.80x | 7.1 ns; 2.93, 37.4 ns; 1.54x |
| 9 | Arena blocks | Partially done | Ending a scope caches its largest block (not in debug mode) for the next block needed; `a.cached` counts it, `held` does not, the limit caps both. Geometric block sizing not done: it would change exact block accounting the tests and `refused` rely on, and the cache already removes the 64 MiB scope churn. The alternating 100 B / 70,000 B case is unchanged (1.93x): fixing it needs dedicated blocks kept behind the head, which would stop large buffers growing in place (finding 2). | 64 MiB scope 24.7 ms, 16,385 faults; 70x1 KiB scope 373 ns | 2.3 ms, 8 faults; 117 ns |
| - | `cu_str_find` worst case | Done, threshold tuned 2026-10-15 | memchr+memcmp as before, switching to Two-Way (O(1) memory) once candidates come more than once per 2 bytes or could cost more than 32 compares per byte scanned: linear on any input. The first rule (8 compares per byte) never switched for needles of 8 bytes or fewer, and switched on DNA-like text, where Two-Way is slower than memchr. Tuned on 4 MiB inputs, best of 3 runs of best of 5, under load about 9: text unchanged (0.29 ns/byte); a^N with an 8-byte needle 4.19 to 0.94; (ab)^N, needle `ac`, 2.07 to 1.07; DNA with a 64-byte needle 2.00 to 1.15. | a^256 b: 5.96 ns/byte (memmem 2.22) | 0.78 ns/byte |
| - | `cu_sb_long` reserves twice; `cu_sb_char` no fast path | Done | See 3 and 7. | | |
| - | `find()` not inlined | Done | See 4. | | |
| - | `cu_input_read` chunk size | Documented | `cu.h` notes 64 KiB or larger chunks; read strategy unchanged. | | |

Costs: `cu_temp_begin`/`cu_temp_end` save and restore the scope's start
position (two more fields in `CuArena` and `CuTemp`), and the arena keeps
a cached block; an empty scratch scope measured 9.2 ns before and 11.6 ns
after. `CuArena`, `CuTemp` and `CuPool` gained fields; the API is source
compatible.
