# Subprocesses

`CuProcess` starts an executable directly, waits for its exit, and requests
termination. The POSIX backend uses `posix_spawn`; other backends return
`ENOSYS` from start. This is not a shell, sandbox or process-tree manager.

## Ownership

A successful start, with or without redirection, creates one arena-owned handle
for one direct child. Keep every successful handle reachable and wait for `CU_PROCESS_DONE`
before releasing its arena or ending the scope containing it. Arena cleanup
does not kill or reap a process. Failed starts can consume arena space but
leave the output pointer unchanged.

Serialize operations on each handle. Different handles may be used by
different threads with separate allocator state. Do not change child-reaping
policy during start or while any handle is live. In particular:

- Do not ignore `SIGCHLD` or enable `SA_NOCLDWAIT`. Start rejects those initial
  settings with `EINVAL`, without changing the parent's policy.
- Do not reap an owned child elsewhere. A handler using `waitpid(-1, ...)`
  can violate this rule even if it was written for another subsystem.
- An unexpected `ECHILD` while waiting aborts. The ownership contract was
  violated; continuing to signal that PID could target an unrelated process.

This is ordinary C resource ownership, not enforced automatic cleanup. The
regions checker has no borrowing model for `posix_spawn`, so start has no
regions summary. Scoped arguments can produce conservative diagnostics;
passing a check is not proof that the arena outlives the child.

## Starting

```c
int cu_process_start(CuProcess **out, CuArena *arena, const char *path,
                     char *const argv[], char *const envp[]);
```

- `path` names the executable; it can be relative to the inherited working
  directory. There is no PATH search or implicit shell.
- `argv` is NULL-terminated and contains `argv[0]`. Spaces, quotes, wildcards
  and shell metacharacters remain literal argument bytes.
- `envp` is a NULL-terminated environment vector. **NULL means an empty
  environment**, not implicit inheritance. Supply needed variables explicitly.
- Vectors and NUL-terminated strings are borrowed only during start.
- Plain start inherits cwd, standard streams and other descriptors not marked
  close-on-exec. Caller descriptor ownership is unchanged. Use the redirected
  variant below to select different child standard streams.
- The child has an empty signal mask and default catchable dispositions.
  The parent's signal mask and dispositions are unchanged.

Do not change process-global cwd or mutate borrowed argument/environment
storage concurrently with start. The API does not change either global state.

Return is 1 with child ownership, or 0 with `errno`. Allocation refusal is
`ENOMEM`. POSIX spawn functions return error numbers rather than necessarily
setting `errno`; the wrapper makes the public error visible in `errno`.

Start is not deadline-bounded. POSIX permits some child-side setup or exec
failures to appear as exit 127 after a successful start. That is indistinguishable
from a program deliberately exiting 127. Success establishes child ownership,
not proof that the requested program entered `main`.

## Per-child standard streams

```c
#define CU_PROCESS_INHERIT (-1)
typedef struct CuProcessStdio {
    int input;
    int output;
    int error;
} CuProcessStdio;
int cu_process_start_redirected(CuProcess **out, CuArena *arena, const char *path,
                               char *const argv[], char *const envp[],
                               const CuProcessStdio *stdio);
```

Initialize every field to a borrowed native descriptor or `CU_PROCESS_INHERIT`.
The record is borrowed only during start. Keep its fields and source descriptor
identities stable throughout that call. After success, the child has its own
references, so the caller can close its originals without invalidating the
child's streams. The parent standard streams and original descriptor flags do
not change.

Each explicit source means the original object, even for swaps or cycles involving
0, 1 and 2. Explicit sources work with `FD_CLOEXEC`, including a source already
at its destination number. Selecting the same source for output and error merges
those streams. Duplicates share open-file descriptions, so child I/O can advance
parent-observable offsets and shares status flags such as `O_APPEND` or
`O_NONBLOCK`. Flush and position buffered input/output before passing its file
descriptor. Coordinate any later parent I/O and use the stream's positioning
operations to synchronize it before reading child output through `FILE`.

`CU_PROCESS_INHERIT` preserves plain-start behavior for that stream, including
closed or close-on-exec standard descriptors. This is not a descriptor whitelist.
Other non-CLOEXEC descriptors still inherit, including explicit source FDs above
2. Mark caller sources close-on-exec when their extra child references are unwanted.
Temporary snapshots do not survive successful exec.

Closed/invalid descriptors, including values below -1, fail with `EBADF` on the
native backend. The implementation validates all sources before making copies,
so a temporary FD cannot make a later, originally closed source look valid.
The build needs `F_DUPFD_CLOEXEC` for explicit redirection. Its absence produces
`ENOSYS`, with no racy dup/fcntl fallback; an all-INHERIT configuration needs no
copies. Other native duplication errors propagate unchanged. Required NULL
arguments, including `stdio`, abort. All other start/error/ownership rules apply.

Temporary copies are closed once. Retrying close after EINTR could close a reused
number. On systems where EINTR leaves an FD open, full reclamation is not guaranteed.
Cleanup errors cannot discard an already-spawned child or replace a setup/spawn
failure. Success still establishes ownership, not confirmed exec or a hard cleanup
bound.

This operation does not create pipes, pump input, capture output, suppress caller
write-SIGPIPE, or limit output volume. Pipe users must close unused ends and drain
stdout/stderr concurrently while feeding input. Waiting for exit or draining one
channel first can deadlock. Timed wait does not drain pipes or kill on timeout.
For finite-input communication with bounded binary captures, use
[`CuProcessIO`](PROCESS_IO.md). Its Linux backend supplies pipe ownership,
fair concurrent draining, one communication budget and per-thread SIGPIPE
handling. Signaling and reaping still use the child lifecycle below.

## Waiting and signaling

```c
CuProcessWait cu_process_wait(CuProcess *child, unsigned long timeout_ms,
                              CuProcessExit *out);
int cu_process_signal(CuProcess *child, CuProcessSignal signal);
```

Wait returns:

| Result | Meaning |
|---|---|
| `CU_PROCESS_DONE` | Child reaped; `out` contains its terminal status |
| `CU_PROCESS_RUNNING` | No completion observed during this wait; child still owned |
| `CU_PROCESS_ERROR` | `errno` describes a failure; child still owned |

`timeout_ms=0` is a nonblocking check. A positive value is one monotonic waiting
budget across interrupted waits and sleeps. It is not a child lifetime limit
or a hard bound on scheduling delays. RUNNING and ERROR leave `out` unchanged.
Clock failures preserve the reported error, including zero. A later wait can
retry. DONE caches its result, so repeated waits require no further OS wait.

`CuProcessExit.kind` is `CU_PROCESS_EXITED` or `CU_PROCESS_SIGNALED`. Its `code`
is the exit status or native signal number respectively, not shell-style
`128 + signal` encoding.

`CU_PROCESS_TERMINATE` requests SIGTERM and `CU_PROCESS_KILL` requests SIGKILL.
A successful signal call means submission, not completion; wait afterward.
After DONE, signaling fails with `ESRCH` without a native kill call. A timeout
does not implicitly kill, detach, reap, or discard the child.

There is no process-group or descendant cleanup. A child may create processes
this handle does not own. Credential changes can also make later signaling
fail. Treat executable selection and the child program as trusted policy.

NULL required arguments, a missing `argv[0]`, or an unknown signal value are
programmer errors and abort. Valid pointer, length, termination, lifetime and
exclusive-access obligations remain with the caller.

## Complete C89 example

This example redirects a trusted `/bin/echo` to a temporary file without replacing
the parent's stdout. It waits up to five seconds, then requests SIGKILL on timeout
and continues waiting for the direct child. On success it reads the temporary file
back to stdout. The file can grow; this is file redirection, not bounded concurrent
pipe capture. Cleanup is not hard-deadline-bounded. Unrecoverable wait/signal errors are fatal in this small
program; a service needs its own supervision policy rather than releasing a
live handle's arena.

```c
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "cu.h"

static void fatal(const char *operation)
{
    perror(operation);
    abort();
}

int main(void)
{
    CuArena arena;
    CuProcess *child;
    CuProcessExit status;
    CuProcessWait waited;
    CuProcessStdio io;
    FILE *capture;
    char buffer[256];
    size_t length;
    char executable[] = "/bin/echo";
    char greeting[] = "hello";
    char *args[3];
    int timed_out;
    int result;

    cu_arena_init(&arena);
    args[0] = executable;
    args[1] = greeting;
    args[2] = NULL;
    capture = tmpfile();
    if (capture == NULL)
        fatal("tmpfile");
    io.input = CU_PROCESS_INHERIT;
    io.output = fileno(capture);
    io.error = CU_PROCESS_INHERIT;
    if (io.output < 0 || fcntl(io.output, F_SETFD, FD_CLOEXEC) != 0)
        fatal("descriptor");
    if (!cu_process_start_redirected(&child, &arena, executable, args, NULL, &io)) {
        perror("start");
        if (fclose(capture) != 0)
            fatal("close");
        cu_arena_release(&arena);
        return 1;
    }
    waited = cu_process_wait(child, 5000, &status);
    if (waited == CU_PROCESS_ERROR)
        fatal("wait");
    timed_out = waited == CU_PROCESS_RUNNING;
    if (timed_out) {
        if (!cu_process_signal(child, CU_PROCESS_KILL) && errno != ESRCH)
            fatal("kill");
        do {
            waited = cu_process_wait(child, 1000, &status);
            if (waited == CU_PROCESS_ERROR)
                fatal("reap");
        } while (waited != CU_PROCESS_DONE);
    }
    result = !timed_out && status.kind == CU_PROCESS_EXITED ? status.code : 1;
    if (result == 0) {
        if (fseek(capture, 0L, SEEK_SET) != 0)
            fatal("seek");
        while ((length = fread(buffer, 1, sizeof(buffer), capture)) != 0) {
            if (fwrite(buffer, 1, length, stdout) != length)
                fatal("output");
        }
        if (ferror(capture) || fflush(stdout) != 0)
            fatal("read/output");
    }
    if (fclose(capture) != 0)
        fatal("close");
    cu_arena_release(&arena);
    return result;
}
```

Compile using the repository's C89 integration flags and `src/*.c`. The
example's `/bin/echo` path is a POSIX-host example, not a portable executable
location or Windows backend. A caller can use a different explicit path.

## Verification scope

Native tests use literal child exit codes, exact argument/environment checks,
readiness-pipe signal tests, cached completion, timeout ownership and concurrent
starts from separate threads. A boundary fixture covers allocation/setup/spawn
errors, interrupted waiting, clock errors, elapsed-time arithmetic, output
preservation and suppression of signaling after reaping. Its status oracle is
produced by an actual kernel child exit rather than a guessed bit layout.

The redirection tests add exact binary input/stdout/stderr, merged streams,
explicit CLOEXEC sources, parent-descriptor preservation, early caller closure,
concurrent starts and isolated standard-stream swaps/cycles/closed-stream cases.
Setup fault injection checks every descriptor-copy and file-action acquisition,
including errno zero, cleanup clobbering and preservation of successful child
ownership. The exact environment oracle uses child-only redirection to an
uninstrumented POSIX utility; it does not replace the parent's stdout.

The standard gate checks unsupported backends, compiler/sanitizer profiles and
regions. See the slice's verification receipt for actual results. A clean test
run does not certify subprocess isolation, all POSIX systems, arbitrary signal
handlers, process trees, or hard real-time behavior.
