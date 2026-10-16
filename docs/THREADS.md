# Threads: design note

- Status: slice 1 implemented (src/cu_thread.c, tests/test_thread.c); slice 2 deferred
- Date: 2026-10-15

Roadmap layer 2 asks for threads behind the platform module: start and join,
a mutex, a condition variable and a work queue, with no platform type in
`cu.h`. cgrep hand-rolls all of this on pthreads in `src/parallel.inc`; this
note proposes what moves into c-utils and what stays in cgrep.

## What cgrep's pool does

Two layers are mixed in `parallel.inc`:

- **Plumbing.** Start and join workers; one mutex and a condition variable
  per worker; abort on any pthread error (they mean a bug, not a runtime
  condition); a cancel flag read under the mutex and polled every few
  reads; every started thread joined before the call returns; a serial
  path when threads are unavailable (not glibc 2.34+, or Cerberus).
- **Policy.** Worker `w` takes paths `w, w + jobs, ...`; each worker has two
  1 MiB output slots, so it scans one file ahead of the coordinator; the
  coordinator drains slots in argument order, so output is deterministic;
  non-regular files are handed back to the serial path.

The plumbing is generic. The policy is cgrep's and stays there.

## Proposal: two slices

### Slice 1: primitives

```c
typedef struct CuThread CuThread;  /* opaque, lives in an arena */
typedef struct CuMutex CuMutex;
typedef struct CuCond CuCond;

/* 1 and *t set, or 0 with errno saved: ENOSYS where the platform has no
 * threads (wasm32-wasi, Cerberus), EAGAIN when the OS refuses, ENOMEM when
 * the arena does. stack_size 0 means the platform default. The thread
 * runs fn(context), then releases its scratch arenas. */
CU_MUST_USE int cu_thread_start(CuThread **t, CuArena *a, size_t stack_size, void (*fn)(void *),
                                void *context);
void cu_thread_join(CuThread *t);           /* exactly once; aborts otherwise */

CU_MUST_USE int cu_mutex_init(CuMutex **m, CuArena *a);
void cu_mutex_lock(CuMutex *m);             /* errors abort */
void cu_mutex_unlock(CuMutex *m);
void cu_mutex_destroy(CuMutex *m);

CU_MUST_USE int cu_cond_init(CuCond **c, CuArena *a);
void cu_cond_wait(CuCond *c, CuMutex *m);
void cu_cond_broadcast(CuCond *c);
void cu_cond_destroy(CuCond *c);

/* Test hook, as cu_arena_fail_at: start and init attempt n (counting from
 * 0, process-wide) fails with EAGAIN, every other one succeeds. Set it
 * and make the attempts from one thread. (size_t)-1 fails none. */
void cu_thread_fail_at(size_t n);
```

- **Opaque storage in the caller's arena.** `cu.h` names no pthread type.
  The object is pushed into the arena given to `init`/`start`, so it is
  freed with the arena; `destroy` and `join` must come first. (A check at
  release for live objects was considered and dropped: the arena would
  have to track platform objects, for a rule any C resource already has.)
- **Errors.** Starting can fail for real reasons, so it returns 0 with
  errno, like `cu_dir_open`, leaving the output pointer unchanged. Failed
  attempts may consume arena space. Lock, unlock, wait and broadcast abort
  on platform-reported errors; joined and destroyed objects are checked.
  Mutex ownership is not checked, so the caller must not lock recursively,
  unlock or wait without owning the mutex, or destroy objects still in use.
- **No detached threads.** A started thread must be joined; there is no
  `detach`. The thread wrapper calls `cu_scratch_release` before the
  thread ends, so per-thread scratch arenas cannot leak.
- **Stack size is a parameter**, 0 for the platform default. cgrep runs 8
  workers under a 32 MiB address-space limit in a test, and passes
  256 KiB; 8 default 8 MiB stacks would not fit.
- **Fault injection.** cgrep's tests make the Nth `pthread_create`, mutex
  init or cond init fail through macro seams, and its mutation tests depend
  on those paths. `cu_thread_fail_at` keeps them testable once the
  plumbing moves. Configure it only when no start or init is in progress,
  and make attempts from one thread while it is enabled. With injection
  disabled, starts and inits on separate arenas may run concurrently.
- **The wrapper.** `cu_thread_start` pushes a small record (fn, context,
  the platform handle) into the arena, so a thread needs no `malloc`; it
  stays there until the arena is released, and `join` ends its use.
- **Broadcast only.** cgrep waits on conditions that more than one thread
  can be waiting for, and uses only broadcast. `signal` and timed waits
  wait for a caller.
- **No atomics yet.** cgrep reads its cancel flag under the mutex and
  measured no cost from it. Atomics wait for a caller that needs them.
- **Platforms.** POSIX threads where available; where not, `start` and
  `init` fail with ENOSYS, and callers keep a serial path. C11
  `<threads.h>` or Win32 can be added behind the same API.
- **regions.** No contract on `cu_thread_start`, so passing it a value
  from a temp or scratch scope is `rg-unchecked`, which matches REGIONS.md's
  rule for thread creation.

### Slice 2: not now

The cgrep session's review (2026-10-15): cgrep's scheme is specialised
(static assignment, two byte-stream slots per worker, ordered drain,
serial fallback decided after a worker opens the file), and a general
ordered queue would have to carry bounded byte streams to fit it. Revisit
after cgrep runs on slice 1. The shape considered:

A queue of `count` items processed by `jobs` workers whose results are
consumed in item order, with a bounded number of results in flight and
cancellation: the general form of cgrep's scheme, without its channel
bytes or file policy. Each item carries an opaque value of the caller's,
as cgrep's paths now carry a per-path `walked` flag (a62cd05); the queue
passes it through without reading it. It waits until slice 1 has landed and cgrep has
moved its plumbing onto it, so the second user shapes the API.

## Linking and the platform gate

The gate is decided at compile time: `src/cu_thread.c` uses POSIX threads
on Unix-like targets other than wasm32-wasi and Cerberus, and otherwise
compiles to the ENOSYS versions. There is no runtime probe.

Consumers link nothing extra on glibc 2.34 and later, where pthreads live
in libc (cgrep's build has no `-pthread` today), or on musl (the i386
check). Older glibc needs `-pthread`; dcheck's builds do not pass it, so
there the tests fail to link rather than silently skipping.

## Checks

- **Deterministic tests** run every primitive with one thread and a fixed
  schedule (start, join, lock around shared counters, wait for a flag).
- **Fault paths** step `cu_thread_fail_at` through every start and init,
  as the arena tests step `cu_arena_fail_at`.
- **Platform failures** inject attribute init, stack setup, thread create,
  mutex init and condition init errors. Cleanup runs only for initialized
  attributes, and failure preserves the output pointer and errno.
- **Real concurrency** gets a counter stress test and concurrent starts
  and inits on worker-owned arenas. The toolchain adds a `tsan` check with
  slice 1, in the shape of cgrep's `parallel_check`: the tests under TSan
  plus a seeded deliberate race that TSan must report.
- **wasm** has no threads: the tests check the ENOSYS path there, as under
  Cerberus. i386 (musl) and valgrind run the threaded tests.
- **CBMC** is not used for this slice.

## Review answers (cgrep session, 2026-10-15)

1. Arena-held opaque objects: fine. cgrep keeps `malloc` only for its slot
   memory, which stays in cgrep.
2. Stack size: required, as above.
3. Slice 2: not now, as above.
