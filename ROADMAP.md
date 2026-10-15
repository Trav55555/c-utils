# Roadmap

Each item is one slice that passes `make check` (and `make deep` before it is called done), with tests written first. Later layers only use the layers below them.

## 1. Core (this repo)

Done: arena + temp scopes + scratch arenas, debug arena mode, checked size math, `CuStr`, ASCII, parsing, string builder, `CuVec`, `CuSlice`, `CuMap` hash trie, `CuPool` with generational handles, `CuPlex` typed links, byte readers and writers (8, 16 and 32 bits, both byte orders). 64-bit readers wait for a 64-bit type: C89 has none, and `unsigned long` is 32 bits on Windows and wasm32. Typed slice wrappers (`CuSliceLong` and so on) wait for a caller that needs more than `CU_SLICE_AT`. `CuMap` hashes with 32-bit FNV-1a, unseeded, so colliding keys are easy to construct and cost O(n) each; a seeded or 64-bit hash waits for a use with untrusted keys.

Temp scopes carry ids, so ending one out of order or twice aborts, and each container aborts when grown under a scope other than the one it was made in. The `regions` check below is the static version of that runtime check.

Done: **a first consumer.** cgrep (`~/projects/cgrep`, CI-pinned to a c-utils revision) streams literal searches over explicit files or stdin using only the arena, `CuStr`, `CuVec` and `CuInput`. The growth and map costs it was meant to measure were addressed in the [2026-10-14 performance review](docs/PERFORMANCE-REVIEW-20261014.md): vectors and stream reads grow in place through `cu_arena_grow` (builders only within their block, so finished views stay valid), each map entry is one push with its hash stored, and ended scopes keep a block for reuse. Its Disposition table lists what remains. What cgrep had to build itself is what the layers above lack:

- Layer 2: threads, a worker pool and cancellation (hand-rolled on pthreads in cgrep's `src/parallel.inc`); buffered stdout (cgrep stages each line's parts into one `fwrite_unlocked` call); directory walking, which cgrep does not have at all, so it searches only the paths it is given.
- Layer 3: a vectorised literal finder and newline counter (in cgrep's `src/cgrep.c`, using GCC/Clang vector types around libc `memchr`).

Next, in order:

1. **`regions` check.** Static check for arena lifetime bugs (design: `dependable-c-agent-toolchain/docs/REGIONS.md`). This repo and cgrep are its first real targets.
2. **The next platform slice**, chosen between threads (moving cgrep's worker pool here) and a directory walk (which cgrep needs before it can search directories).

## 2. Platform module

The only code allowed to touch the OS. Everything above it stays portable C89.

- Files: read whole file into an arena, buffered reader/writer, directory walking.
- Threads: start/join, mutex, condition variable, a work queue (see `dependable-c-agent-toolchain/docs/CONCURRENCY.md`). Shared flags and pointers use atomics with acquire/release ordering (C11 `<stdatomic.h>` or compiler builtins, wrapped here), never `volatile`: `volatile` stops the compiler eliding accesses but gives neither atomicity nor ordering.
- Time, environment, process exit codes. Effects are visible to Ward's `authority` check.

## 3. Tools: ripgrep-style search

The proving ground for "safe and fast". Needs, beyond the core:

- `memchr`-driven line splitting and literal search (libc's `memchr` is SIMD; keep it).
- A regex engine: Thompson NFA first, then a lazy DFA. Linear time by construction, no backtracking blowups.
- `.gitignore`-aware directory walk, and a thread pool over files.

Benchmark against `rg` on a fixed corpus. The target is within 2x before optimising further.

## 4. Web stack

Server in C, browser logic in C compiled to wasm, HTML over the wire in the style of htmx and Datastar.

- **HTTP/1.1 server** on the platform module: request parser fuzzed and CBMC-proved on small inputs, arena per request (freeing a request is one `cu_arena_release`).
- **Routes as data.** A route table (C89 struct arrays) declares method, path, parameters and their types, response shape and docs text. The same table drives dispatch, parameter parsing/validation, and **generated OpenAPI JSON plus a docs page**, the way FastAPI does from type hints. One source, so the docs cannot drift.
- **HTML builder** on `CuBuilder` with escaping by default: text is escaped unless explicitly marked raw, removing XSS as a bug class.
- **Server-driven UI:** responses are HTML fragments; Server-Sent Events push fragment and signal updates (Datastar-style). A small client runtime handles swapping.
- **wasm where JS would be:** shared C validation and formatting code compiled for the server and for `wasm32` (dcheck's `wasm` check already proves the code builds and runs there). Keep JS to the minimal glue for DOM access.

## 5. TUI library

- Candidate: [Clay](https://github.com/nicbarker/clay) for layout. Clay is C99, and its API depends on designated initializers and compound literals, so it cannot be used from C89 code directly. Options: wrap it in an adapter compiled as C99 behind a C89 API, or write a smaller C89 flexbox-style layout. Decide with a prototype.
- Terminal backend in the platform module: raw mode, alternate screen, double-buffered cell grid, only changed cells written.
- Immediate-mode API: the UI is a function of state, rebuilt each frame in a scratch arena.

## Developer experience (humans and agents)

- One command for everything: `make check`, with `dcheck --json` and `dcheck explain` for agents.
- Every module documents its contract at the top of its header, with an example in the README.
- Diagnostics say what to do: `regions` messages follow Rust's shape (location, notes, `help:`).
- Each primitive ships with tests, a fuzz harness or proof where it makes sense, and a worked example.
