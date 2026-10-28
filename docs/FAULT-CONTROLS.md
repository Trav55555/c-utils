# Fault controls

A passing test shows that the code meets a rule on the inputs tried. It does
not show that the test would notice the rule being lost. A fault control
supplies that evidence: it seeds one known fault into a scratch copy of the
sources and requires the focused test, fuzz input or proof to fail on its
contract check.

```sh
make controls DCHECK=/path/to/dcheck
```

[`tools/fault_controls.py`](../tools/fault_controls.py) copies `include/`,
`src/`, `tests/`, `fuzz/`, `proofs/`, `Makefile` and `dcheck.conf` to a
temporary directory for each control and edits only that copy. It first runs
every selected check on an unmodified copy, which must pass. Tests build with
the Makefile's rule. The fuzz harness and CBMC proof build with the commands,
flags and `dcheck.conf` settings that dcheck's `check_fuzz` and `check_cbmc`
use. The make target runs the whole job under `tools/verify_deep.py`, with a
900-second limit, a 3 GiB memory cap and swap disabled. Each test run has
30 seconds, and each CBMC run has 600 seconds. Two full runs took 46 and
79 seconds on a Ryzen 5 7600 under a load average of about 8.

Each line of output reports one check:

| Output | Meaning |
|---|---|
| `PASS baseline` | The unmodified copy passes this check. |
| `CAUGHT` | The faulty copy builds and the check fails with exactly the expected assertion. For a proof, the set of failed properties must match exactly. |
| `UNREACHED` | The control records that this check cannot reach the fault, so the check must still pass. |
| `SURVIVED`, `WRONG FAILURE`, `BROKEN`, `REACHED` | The fault went undetected; the check failed some other way (another assertion, a build error, a CBMC stop or a timeout); or a check that was recorded as unable to reach the fault failed. Each of these fails the run. |
| `stale control` | The faulted source text no longer appears exactly once. The run stops. |

`tests/test_verify_controls.py` runs in both CI workflows. It checks that every
fault still applies to the current sources, without needing a compiler,
fuzzer or CBMC.

The controls are single representative faults, not a mutation score. A caught
control shows that one way of losing a rule is detected. It does not show that
every way is detected.

## Scope and stale-handle controls

| Control | Fault (scratch copy only) | Check | Failing assertion | Invariant |
|---|---|---|---|---|
| `release-keeps-base-scope` | `cu_arena_release` sets `a->scope = 1`, the base id from `cu_arena_init`, instead of `next_scope_id(a)`. Commit `63f0bb1` removed this behavior. | `test_scopes` | `tests/test_scopes.c:35: test_scope_ids_nest: Assertion 'cu_arena_scope(&a) != base && cu_arena_scope(&a) != outer.id' failed.` | Each release issues a base scope that differs from the old base and from every scope that was open. |
| `release-keeps-base-scope-stale-vector` | The same fault. The copy also deletes the assertion above, so only the behavioral death test remains. | `test_scopes` | `expect_abort: vec_push_after_release exited with status 0`, then `expect_abort_named: Assertion 'WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT' failed.` | A container made before a release aborts when it is grown after the release. Under the fault, the push wrote into the freed block and the child exited normally. |
| `pool-ignores-generation` | `live_slot` in `src/cu_pool.c` stops comparing `s->gen` with `h.gen`. | `test_pool` | `tests/test_pool.c:48: test_reuse_bumps_generation: Assertion 'CU_POOL_GET(&p, long, old) == NULL' failed.` | A removed object's handle stays stale after its slot is reused. |
| | | `test_plex` | `tests/test_plex.c:88: test_removed_node_links_go_stale: Assertion 'CU_PLEX_GET(&px, Person, person, y) == NULL' failed.` | A link to a removed node stays stale after the node's slot is reused. |
| | | `fuzz_pool`, input bytes `02 01 00 01` | `ERROR: libFuzzer: deadly signal` (a `__builtin_trap` in the model comparison) | The harness compares each handle with its independent live-object model. The input selects `max_gen` 2, then adds an object, removes it and adds again into the same slot. |

The fuzz input reaches the fault only through slot reuse. On the faulty copy,
`02 01 00` (no second add) and `00 01 00 01` (`max_gen` 1, so the slot retires
instead of being reused) both pass. The trap's line is not reported because
`-O1` merges the harness's traps. The model comparison is the only check that
the third step can fail.

Both of these controls were caught by existing tests, so neither exposed a coverage
gap and no regression test was added. One diagnostic change was needed to
identify the failing death test: `tests/death.h` now names a body that does
not abort, and says how it ended, before its assertion fails. The assertion
alone does not say which of a file's death tests failed.

### Limits these checks do not cover

- **Raw pointers.** Scope ids guard container operations, and generations
  guard handle lookups. Neither guards a raw pointer. A pointer from
  `cu_arena_push`, `CU_VEC_AT`, `CU_MAP_GET`, `CU_POOL_GET` or `CU_PLEX_GET`
  that is used after its scope ends, after the arena is released, after its
  object is removed or after a grow moves it is not caught by these checks.
  Debug-mode arenas under the sanitizers may catch some of these uses.
- **Arena re-initialization.** `cu_arena_init` restarts scope ids, so a
  container or `CuTemp` made before re-initialization can match again. The
  header says not to re-initialize an arena that still has users. No check
  catches this, so no control can fail on it. A global generation counter
  would catch it but would need atomics, which C89 lacks.
- **Size-only macro types.** `CU_VEC_AT`, `CU_MAP_GET`, `CU_POOL_GET`,
  `CU_PLEX_GET` and the other typed macros compare `sizeof(T)` with the
  stored size. A different type of the same size, such as `long` and
  `double` on LP64, passes.
- **Death tests.** A death test accepts any `SIGABRT`. If a heap-consistency
  abort happened in place of the contract abort, the test would still pass.
  The stale-vector control requires the child to exit normally under the
  fault, so it would report `WRONG FAILURE` in that case.

These checks are not evidence of general memory safety.

## Arena-growth failure evidence

### The contract

`include/cu.h` states the failure promise of `cu_arena_grow`:

> On failure it returns NULL, marks the arena failed, and p keeps its bytes.

For the reallocation path, it adds: "one block allocation attempt
(cu_arena_fail_at), held grows by the bytes added, the limit applies to
those bytes, and a refusal records them in refused." The push-and-copy path
fails with "the same failure rules as cu_arena_push". The header also says
that memory lives in the arena until the arena is released.

`cu_arena_grow` can fail in these ways:

1. **In place** (`cu_arena_extend`). This path never allocates and never
   fails.
2. **Block reallocation** (`grow_head`), when `p` is alone in the newest
   block and that block was made under the innermost scope. Before `realloc`
   runs, it refuses if the header plus size overflows, if the attempt is the
   one `cu_arena_fail_at` selected, or if the limit would be passed. After
   that, `realloc` itself can return NULL.
3. **Push and copy**: a `cu_arena_push` failure. The causes are overflow,
   `cu_arena_fail_at`, the limit or `malloc` returning NULL.

### What covers each promise

| Promise | Path | Tests (`tests/test_arena.c`) | Proof (`proofs/proof_arena_grow.c`) | Control |
|---|---|---|---|---|
| NULL and the arena marked failed | `realloc` returns NULL | Not reached | `a failed grow marks the arena` | `grow-failure-unmarked` |
| | `cu_arena_fail_at` refuses | `test_grow_failure`, lines 469–470 | Not reached | `grow-injected-failure-unmarked` |
| | Limit refuses | `test_grow_failure`, lines 478–479 | Not reached | `grow-limit-refusal-unmarked` |
| | Size overflow | `test_grow_failure`, line 495 | Not reached | — |
| | Push and copy | `test_grow_failure`, line 490 | Not reached | — |
| `held` unchanged by a failure | `realloc` returns NULL | Not reached | `a failed grow holds nothing more` | `grow-failure-counts-held` |
| | Injection or limit | `test_grow_failure`, lines 470 and 479 | Not reached | — |
| `p` keeps its bytes | `realloc` returns NULL | Not reached | `a failed grow keeps the old bytes`, at a symbolic position | `grow-failure-clears-bytes`, `grow-failure-clears-one-byte` |
| | Injection or limit | `has_pattern(p, 64)`, which checks all 64 bytes, lines 470 and 479 | Not reached | — |
| The arena still owns the block | `realloc` returns NULL | Not reached | `release returns every held byte` | `grow-failure-drops-block` |
| A limit refusal records the bytes it needed | Limit refuses | `test_grow_failure`, line 479 | Not reached | `grow-limit-refusal-unrecorded` |
| On success, `held` grows by the bytes added and the bytes move | Reallocation | `test_grow_reallocates_own_block` | `held grows by the bytes added`, `the bytes move with the grow` | `grow-success-clears-one-byte` (caught by both) |

**Allocation injection versus the modeled `realloc` branch.**
`cu_arena_fail_at` and `cu_arena_set_limit` refuse in `grow_head` before
`realloc` runs, so the tests check every refusal except a NULL from
`realloc` itself. On a real allocator, a NULL needs a request too large for
the sanitizers to grant, and no test makes one. The proof covers only that
branch. It never sets a failure point or a limit, so the injected and limit
refusals are unreachable in it. The `UNREACHED` lines confirm this in both
directions. Every `grow-failure-*` fault passes `test_arena`, and all three
injection and limit faults pass the proof.

**A gap in the tests.** Before this change, no test checked that a limit
refusal on the reallocation path marks the arena failed. With
`a->failed = 1` removed from that branch, `test_arena` still passed
(`SURVIVED grow-limit-refusal-unmarked`). `test_grow_failure` now also asserts
`cu_arena_failed(&a)` after that refusal, at line 479. The control is caught
there, and the unmodified source passes.

### The proof's model and bounds

- **Fixed sizes.** The block size is 16 (`OLD`), one 16-byte push fills the
  block, and the push grows to 40 bytes (`NEW`). Debug mode is
  nondeterministic, so both arena modes are checked. Because the allocation
  is alone in its block, only the reallocation path runs. The proof does not
  exercise in-place growth, push and copy, overflow, limits, injection, temp
  scopes, the cached block, several blocks or other sizes.
- **Allocator replacement.** `malloc` is `__CPROVER_allocate` and never
  fails. `realloc` fails nondeterministically. When it succeeds, it
  allocates `n` bytes, copies the smaller of the old object's size and `n`,
  and calls CBMC's `free` on the old object. A failed `realloc` leaving the
  old object untouched is the C library's guarantee, which the model assumes
  and does not check. The real allocator is not checked.
- **Byte checks.** Before the grow, a symbolic value is written at a
  symbolic position below 16. After the grow, that position is compared in
  the old block if the grow failed, or in the new one if it succeeded. CBMC
  checks the comparison for every position and every value, so every one of
  the 16 old bytes is covered at this size, without a loop over them. Only
  one byte is compared in each trace, and bytes beyond 16 have no promise
  and are not checked. At commit `4a8b0f3` and before, the proof compared
  only the first and last bytes, and the `grow-*-clears-one-byte` controls survived
  it.
- **Leaks.** dcheck passes `--memory-leak-check`, but it has no effect here.
  CBMC records leaks in its own `malloc` model, and the harness's allocators
  call `__CPROVER_allocate` directly. The `grow-failure-drops-block` fault is
  the classic `p = realloc(p, n)` bug. It loses the block on failure, and it
  passed the proof at `4a8b0f3`. The added assertion
  `release returns every held byte` catches it. That assertion shows that
  `held` returns to 0, not that every object was freed.
- **Tool and target.** CBMC 6.11.0 ran on its default target for this host:
  x86-64 Linux, LP64, little-endian. With that target, the block header is
  32 bytes and `CU_ALIGN` is 16. The flags are exactly those of dcheck's
  `check_cbmc`:

  ```
  --bounds-check --pointer-check --signed-overflow-check --pointer-overflow-check
  --undefined-shift-check --div-by-zero-check --memory-leak-check
  --unwind 8 --unwinding-assertions --object-bits 8 --trace --compact-trace
  ```

  The proof links all of `src/*.c` and adds `-I include -I <dcheck>/lib`.
  dcheck does not pass `PROJECT_DEFINES` to CBMC. The harness has no
  `dcheck-unwindset`, so every loop is bounded at 8. The only unwinding
  assertion is for the loop in `free_blocks_until`, and it passes.
  `CBMC_TIMEOUT` is 600 seconds. This evidence came from toolchain
  `a5a078a`. CI pins `184f48d`, whose `check_cbmc` uses the same flags
  except `--object-bits`, and 8 is CBMC's default there.
- **Measured.** Three baseline runs took 2.6 to 2.7 seconds wall time and
  142 to 145 MiB peak RSS (`/usr/bin/time`, inside the 3 GiB scope), with
  12,630 properties and none failed. The proof at `4a8b0f3` took 2.2 seconds
  and 131 MiB.

The proof is bounded evidence for one consequential branch at one size on
one target. It does not verify the library as a whole.

## Qualifying a pinned revision

A consumer that builds against a particular c-utils revision, such as a
compiler that builds it from a sibling checkout through `CUTILS` and `DEPS`,
re-qualifies that revision with these commands:

```sh
git -C c-utils fetch
git -C c-utils checkout --detach REV
git -C c-utils rev-parse HEAD
make -C c-utils test
make -C /path/to/toolchain regions/regions
make -C c-utils check DCHECK=/path/to/toolchain/bin/dcheck
make -C c-utils controls DCHECK=/path/to/toolchain/bin/dcheck
cbmc --version
```

Then build and test the consumer against that checkout. Record the c-utils
revision, the toolchain revision and the CBMC version together, because the
proof result depends on all three. The revision qualifies when `make test`
passes, `make check` reports `PASS regions` and `dcheck gate: PASS`,
`make controls` reports `fault controls: PASS`, and the consumer's own tests
pass against the checkout. dcheck skips regions when its binary is missing,
so build it first, as CI does. Any `SURVIVED`, `WRONG FAILURE`, `BROKEN`, `REACHED` or
`stale control` line, or a skipped tool, leaves the evidence for that
revision unresolved, not passed. `make deep` adds Cerberus, Valgrind,
fuzzing and every proof, and private CI runs it nightly.

Revisions before the controls were added do not have them. Running a later
`tools/fault_controls.py --root OLD_TREE` against such a revision either
applies each fault or stops with `stale control`. To demonstrate the two proof
gaps described above, run the grow controls against an archive of `4a8b0f3`
with `--root`. Three controls report `SURVIVED`, and `grow-failure-counts-held`
reports `WRONG FAILURE`, because that proof has no release assertion.
