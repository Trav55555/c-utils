---
id: c-utils-vm-writer-considerations-20261017
tags: [c, design-review, ownership, performance, testing]
type: design-note
status: proposed
created: '2026-10-17'
---
# VM writer lessons for c-utils

Cliff Click's [Bits of Advice for the VM Writer](https://www.youtube.com/watch?v=Hqw57GJSrac) is useful here as a review of interacting contracts. It is not a reason to add a virtual machine, garbage collector, or scheduler to this library.

These notes are for consideration. They describe existing mechanisms and possible checks, not approved implementation work. No code, tests, or benchmarks ran for this note. The source inspection used the current working tree, including unrelated in-progress changes, rather than only a committed release.

## Published views constrain allocation optimizations

The builder already demonstrates the central lesson. Before `cu_sb_finish` publishes a view, growth may reallocate its current buffer. After publication, growth must preserve the bytes behind that view. The builder can extend the buffer without moving it or copy into a fresh allocation. After a copy, the old published view still refers to the retained old allocation.

Evidence is in `reserve` and `cu_sb_finish` in [cu_fmt.c](../src/cu_fmt.c), `test_finished_views_survive_growth` in [test_fmt.c](../tests/test_fmt.c), and disposition item 2 of the [performance review](PERFORMANCE-REVIEW-20261014.md). The public contract is in [cu.h](../include/cu.h).

Consider the same question before optimizing another container: has it handed out a view or pointer whose validity the API promises to preserve? Arena ownership does not make every allocation immovable. `cu_arena_grow` may invalidate the old allocation, and vectors explicitly permit growth to invalidate element pointers.

For a future growth change, preserve a previously returned view through enough appends to force allocation, then compare its original bytes. Exercise allocation failure and self-append where supported. Existing builder tests already cover these behaviors. Extend those tests only for a new transition or a demonstrated gap.

## Make rare paths routine in tests

`cu_arena_fail_at` fails one numbered block-allocation attempt. This differs from a memory cap, which can fail every later allocation and hide an ignored error. Debug arena mode gives each push a separate allocation so sanitizers can see object boundaries. `cu_thread_fail_at` makes thread startup and synchronization-object initialization failures reproducible.

See their contracts in [cu.h](../include/cu.h), `test_append_fails_at_every_allocation` in [test_fmt.c](../tests/test_fmt.c), and the fault-path discussion in [THREADS.md](THREADS.md).

For the next multi-resource API, consider a failure sweep through every acquisition step. Check which outputs must remain unchanged, which failures are sticky, and which resources need explicit cleanup. Join workers and destroy synchronization objects before ending the scope that stores their records. An arena release does not close operating-system resources.

This applies Click's recommendation to force uncommon paths to execute. It does not require a new generic fault-injection framework. Use the existing hooks and the repository's normal verification gates.

## Generational handles are not capabilities or GC roots

`CuPool` stores objects in fixed pages and invalidates handles after removal by advancing the slot generation. `CuPlex` adds kind-checked links. Neither traces reachability, moves live objects, or keeps an arena alive. A raw pointer returned from a pool lookup still needs lifetime discipline.

See the pool and plex contracts in [cu.h](../include/cu.h) and their implementations in [cu_pool.c](../src/cu_pool.c) and [cu_plex.c](../src/cu_plex.c).

If a future plugin host uses these containers, keep permission checks at that host boundary. A guessed valid index and generation is not proof that the caller owns the object. The host needs instance ownership, permitted operations, and any required revocation rules. Do not expose an internal `CuHandle` as an unforgeable security capability.

A future moving collector would need a separate reference and rooting protocol. Adding one is not proposed here.

## Preserve the split between mechanisms and policy

The thread API supplies start, join, mutexes, and conditions. cgrep owns worker assignment, cancellation policy, and ordered output. The [thread design](THREADS.md) and [roadmap](../ROADMAP.md) deliberately defer a generic queue and atomics until a caller establishes a need.

Keep that boundary when evaluating VM-related work. A consumer's safepoints, supervision, and scheduling rules do not belong in c-utils solely because the library supplies its storage or threads. Worker-owned arenas remain preferable where they avoid sharing. A shared container still requires synchronization.

## Measure the complete consumer path

The [performance review](PERFORMANCE-REVIEW-20261014.md) already connects cross-translation-unit calls, optional fast macros, and link-time optimization. Its timings are indicative measurements under load, not portable guarantees.

For a future fast path, compare ordinary separate-source builds and optimized consumer builds. Keep error handling, lifetime checks, and cleanup in the comparison. Retain a specialized path only if a real caller benefits without weakening the public contract.

Click's register-allocation discussion concerns compiler-generated code. It does not by itself justify more macros, a new integer representation, or changing C89 portability policy.

## Questions for the next API review

- Who owns the bytes, and who may retain a reference?
- Which operations invalidate a view, pointer, or handle?
- Can a callback or worker outlive the scope that stores its context?
- What happens after failure at each acquisition step?
- Which existing test can force that transition?
- Does the measured consumer still improve with the promised behavior intact?

The immediate recommendation is to use these questions during existing work. There is no new runtime subsystem or required roadmap slice in this note.
