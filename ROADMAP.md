# Roadmap

Each item is one slice that passes `make check` (and `make deep` before it is called done), with tests written first. Later layers only use the layers below them.

## 1. Core (this repo)

Done: arena + temp scopes + scratch arenas, debug arena mode, checked size math, `CuStr`, ASCII, parsing, string builder.

Next, in order:

1. **`CuSlice` and `CuVec`.** Typed views and growable arrays over `elem_size`, bounds-checked `at`, arena-backed growth. Thin typed wrappers for hot types.
2. **Byte readers and writers.** `cu_rd_u32le(CuStr, &pos, &out)` and friends: bounds-checked, no pointer casts, so no aliasing or alignment UB in binary formats.
3. **Hash trie map keyed by `CuStr`.** Arena-backed (Wellons' design): no rehash, stable entry pointers, nothing freed individually.
4. **Pool with generational handles.** `{index, generation}` handles for long-lived objects: a stale handle is reported, never dereferenced.
5. **`regions` check.** Static check for arena lifetime bugs (design: `dependable-c-agent-toolchain/docs/REGIONS.md`). This repo is its first real target.
6. **Plex.** Linked structure in the Ross/Knuth sense: nodes whose fields point at each other in any shape (graphs, cycles, back links), not just lists or trees. Nodes live in an arena; links are typed indices rather than raw pointers, so a dangling or wrong-type link is reported, never followed. Builds on the arena and on the pool's generational handles (item 4).

## 2. Platform module

The only code allowed to touch the OS. Everything above it stays portable C89.

- Files: read whole file into an arena, buffered reader/writer, directory walking.
- Threads: start/join, mutex, condition variable, a work queue (see `dependable-c-agent-toolchain/docs/CONCURRENCY.md`).
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
