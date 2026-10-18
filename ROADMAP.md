# Roadmap

Each item is one slice that passes `make check` (and `make deep` before it is called done), with tests written first. Later layers only use the layers below them.

For consideration: [VM writer lessons](docs/VM-WRITER-CONSIDERATIONS-20261017.md) reviews escaped views, failure injection, handle limits, and the boundary between library mechanisms and consumer policy. It adds no approved slice and does not change the order below.

## 1. Core (this repo)

Done: arena + temp scopes + scratch arenas, debug arena mode, checked size math, `CuStr`, ASCII, parsing, string builder, `CuVec`, `CuSlice`, `CuMap` hash trie, `CuPool` with generational handles, `CuPlex` typed links, byte readers and writers (8, 16 and 32 bits, both byte orders). `CuU64` stands in for a 64-bit type, which C89 lacks (and `unsigned long` is 32 bits on Windows and wasm32): two 32-bit halves with checked add, subtract and compare, `size_t` conversions, and 64-bit readers and writers. Multiplication, division and decimal formatting wait for a caller; the first is the git server in `~/projects/git-server-agents`. SHA-256 (streaming and one-shot) is there for that server's content addressing; SHA-1 follows when its git view needs it. Typed slice wrappers (`CuSliceLong` and so on) wait for a caller that needs more than `CU_SLICE_AT`. `CuMap` hashes with 32-bit FNV-1a, unseeded, so colliding keys are easy to construct and cost O(n) each; a seeded or 64-bit hash waits for a use with untrusted keys.

Temp scopes carry ids, so ending one out of order or twice aborts, and each container aborts when grown under a scope other than the one it was made in. The `regions` check below is the static version of that runtime check.

Done: **a first consumer.** cgrep (`~/projects/cgrep`, CI-pinned to a c-utils revision) streams literal searches over explicit files, stdin and directory trees. It uses `CuInput` for input and `CuDir` for its own recursive walk. Ignore-file handling and bounded, ordered file parallelism stay in cgrep. The growth and map costs it was meant to measure were addressed in the [2026-10-14 performance review](docs/PERFORMANCE-REVIEW-20261014.md): vectors, stream reads and builders grow in place through `cu_arena_grow` (builders only until they hand out a finished view, which then stays valid), each map entry is one push with its hash stored, and ended scopes keep a block for reuse. Its Disposition table lists what remains.

Commit `ef33d78` ships arena-owned `CuThread`, `CuMutex` and broadcast `CuCond`, with stack-size selection, automatic worker scratch cleanup and startup fault injection. The toolchain supplies a TSan gate and a deliberate-race fixture. The pinned c-utils deep CI passed, including regions and TSan. These checks do not prove all lifetimes or thread schedules.

cgrep's thread migration shipped as `547171a`. Follow-up `bf6c005` pins its CI reference to checksum-validated ripgrep 15.2.0, resolving the older reference's ignore-rule differences without changing production C or test budgets. Pinned acceptance passed all 12,000 ordered comparisons and 2,000 ignore comparisons. cgrep's scheduling, cancellation and two bounded slots per worker remain application policy, not a proposed generic queue.

Remaining consumer code includes per-batch output staging, write-error and flush handling, path classification, and a vectorised literal finder and newline counter. The finder uses GCC/Clang vector types around libc `memchr` and stays in cgrep until another caller establishes a reusable interface.

Next, in order:

1. **Extend `regions` adoption.** This repo enables `REGIONS_CHECK=1`, with one documented `cu_arena_grow` exception. cgrep does not enable the check yet. Audit its contracts and provision the checker before enabling it. Thread creation deliberately has no borrowing model; temp or scratch callback contexts produce `rg-unchecked`, not a scope-safety guarantee. See `dependable-c-agent-toolchain/docs/REGIONS.md`.
2. **Assess path and metadata helpers.** Use cgrep's existing path classification and walking code to identify a small shared contract. Keep traversal and ignore policy in the consumer.
3. **Resolve the `CuOutput` experiment.** Borrowed-FILE and callback-sink prototypes preserved the observed cgrep output and failure cases. The FILE-only interface is smaller. Two 21-round timing runs, including a lower-load repeat, did not resolve the 5% performance gate. Use a longer, controlled comparison before production adoption. Stdio already buffers; another persistent buffer needs evidence. No output API has shipped.

A generic work queue, worker pool or atomics API waits for a caller that needs more than the existing thread APIs.

## 2. Platform module

The only code allowed to touch the OS. Everything above it stays portable C89.

- Files. Whole-file and stream reads, chunked input (`CuInput`), and listing one directory (`CuDir`) are implemented. cgrep recurses on `CuDir` and owns its ignore rules. Durable output is implemented for the git server's write-ahead log and blob store: `CuAppend` (unbuffered appends, fsync, truncate, an advisory lock, sticky errors so a failed fsync is never retried into false success), plus rename, remove, make directory and directory sync. wasm32-wasi has no locks or directory sync and reports ENOSYS for them. Buffered output for stdout (the `CuOutput` experiment below), lexical path operations and portable metadata remain candidates.
- Threads. Start/join, mutex and broadcast conditions are implemented on POSIX. WASI and Cerberus use the `ENOSYS` backend. Join workers and destroy synchronization objects before releasing their arena-owned records. Arenas and containers do not become thread-safe by using them. Shared cancellation state in cgrep stays under its mutex. No atomics or generic queue are implemented; `volatile` is not synchronization. See the [thread design](docs/THREADS.md) and `dependable-c-agent-toolchain/docs/CONCURRENCY.md`.
- Sockets. TCP over IPv4 and IPv6 (`CuSocket`: listen, accept, connect, reads and writes with a time limit, sticky errors, no SIGPIPE) and `CuPoller`, which watches many sockets from one thread with one-shot watches and idle limits, the way a server parks idle keep-alive connections. Its mechanism is private: POSIX `poll()` today, so epoll or kqueue can replace it without changing callers. wasm32-wasi and Cerberus report ENOSYS.
- Time, environment, process exit codes. Effects are visible to Ward's `authority` check.

## 3. Tools: ripgrep-style search

The proving ground for "safe and fast". Needs, beyond the core:

- Implemented in cgrep: `memchr`-driven literal search, SIMD-assisted scanning, recursive directory walking with ignore-file rules, and bounded parallel file search. Its thread-API migration passed the pinned acceptance run above.
- Future work: a regex engine, starting with a Thompson NFA and then a lazy DFA. Linear time by construction, no backtracking blowups. A regex engine is not part of the next c-utils slice.

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
