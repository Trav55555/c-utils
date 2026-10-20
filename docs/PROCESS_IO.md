# Bounded subprocess communication

`CuProcessIO` sends a finite input while draining stdout and stderr. It uses the
existing `CuProcess` for signaling and reaping. The first backend uses Linux
atomic close-on-exec pipes; WASI, Cerberus, macOS/BSD and other unsupported builds
return `ENOSYS` from start. Plain process start and descriptor redirection keep
their existing platform support.

This is one-shot binary communication, not an interactive terminal, resumable
stream, shell or sandbox. Caller buffers limit retained output, not child CPU,
memory, descendants or other external effects.

## Ownership

```c
int cu_process_io_start(CuProcessIO **io, CuArena *arena, const char *path,
                       char *const argv[], char *const envp[]);
CuProcess *cu_process_io_child(CuProcessIO *io);
int cu_process_io_close(CuProcessIO *io);
```

Start uses the [existing executable, environment and reaping rules](PROCESSES.md).
There is no PATH search or implicit shell; NULL `envp` means an empty environment.
Argument/environment storage is borrowed only during start. Start has no deadline.

Return 0 means no successful spawn, with `errno` and unchanged output. Return 1
establishes visible child ownership, not confirmed exec or working I/O. If a
post-spawn close fails, start retains that first I/O error, closes remaining
endpoints, and still returns the child-bearing bundle. The first pump or close
reports the error. This never returns false while silently publishing a child.

The bundle owns three private parent endpoints. The getter borrows its existing
child handle, also after close/failure. Keep the arena and handles alive until
both conditions hold:

- All communication endpoints are closed, by pump or explicit close.
- `cu_process_wait` has reported `CU_PROCESS_DONE` for the child.

Close abandons I/O, not the child. It does not signal or reap. It is idempotent,
including after pump, and replays the aggregate first I/O failure with `errno`.
A failure with errno zero remains a failure. Do not copy a resource handle or
operate on the same bundle/child concurrently. Separate callers need separate
allocator state. Thread cancellation in either mode, asynchronous thread exit and
non-local jumps through resource cleanup are unsupported.

## Fixed buffers and progress

```c
typedef struct CuProcessCapture {
    char *out;
    size_t out_cap;
    size_t out_len;
    char *err;
    size_t err_cap;
    size_t err_len;
    size_t input_sent;
} CuProcessCapture;
int cu_process_io_pump(CuProcessIO *io, CuStr input,
                      CuProcessCapture *capture, unsigned long timeout_ms);
```

Set the two pointers/capacities before calling. Pump resets all three counts.
Pointers may be NULL only for zero length/capacity. Input, output, error and
control storage must be valid and disjoint. There is no portable runtime check
for arbitrary overlap. No buffer or input pointer is retained after return.

Pump feeds input and fairly drains both outputs in bounded, rotating read/write
quanta. Parent endpoints are nonblocking; child standard streams remain blocking.
After accepting all input, it closes stdin's write end so the child sees EOF.
`input_sent` counts bytes accepted by the kernel pipe, not bytes processed by the
child. If the child closes its reader while input remains, pump fails with `EPIPE`.

Output is binary and not NUL-terminated. An exact capacity is allowed if the next
read is EOF. A full buffer triggers a one-byte scratch probe: an extra byte fails
with `EFBIG` and is discarded. A zero-capacity stream therefore accepts empty
output and rejects its first byte. Lengths never include a terminator or discarded
excess byte. POLLHUP does not discard queued data; reads continue to EOF.

On every normal return, all three parent ends are closed. Return 1 means input
was accepted and both outputs reached EOF. It does not mean the child exited or
was reaped. A child can close its streams and continue running. Return 0 preserves
the first errno and valid captured prefixes/input progress. The other output may
have more unread data when a failure stops communication. Save errno before
unrelated work. Closing endpoints can cause child EOF, EPIPE or SIGPIPE.

Pump is one-shot. Calling it twice or after explicit close aborts, as do NULL
required pointers and nonzero lengths/capacities with NULL storage. Use a new
session for another exchange; this API does not resume after timeout or limits.

## Deadline and SIGPIPE

A positive timeout is one monotonic budget starting at pump entry, including
progress, EINTR, EAGAIN and interrupted polls. Timeout fails with `ETIMEDOUT`.
Zero disables the communication deadline; unlike `cu_process_wait(..., 0, ...)`,
it is not a poll. Clock failures propagate, including errno zero. Spawn, later
reaping, OS cleanup and scheduling delays are not hard-return-bounded.

The library temporarily blocks SIGPIPE only in the thread performing a write.
For EPIPE it consumes a newly generated pending SIGPIPE only if none was pending
before that write, then restores the exact original mask. It never changes the
process signal disposition. A previously pending SIGPIPE is left alone. Concurrent
external SIGPIPE can coalesce with the generated signal and cannot be distinguished.
Do not concurrently change SIGPIPE policy or jump/cancel through this operation.
Failure restoring a valid saved mask, or an impossible signal-drain error, aborts
rather than returning with unsafe signal state.

No timeout, output limit or close discards child ownership. Use existing signal
and wait calls afterward. There is no process-group or descendant cleanup; an
inherited descendant writer can keep a pipe open until the communication budget
expires. This API does not enforce filesystem, credential or tenant isolation.

## Complete C89 example

This trusted `/bin/cat` example feeds six bytes, retains at most 64 bytes per output,
and gives communication one second. It reaps explicitly before forwarding captured
prefixes to parent stdio. The kill/reap loop has no hard cleanup bound. Fatal wait
or signal errors abort this small program; a service needs a supervision policy
that keeps ownership instead of releasing a live child's arena.

```c
#include <errno.h>
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
    CuProcessIO *io;
    CuProcess *child;
    CuProcessCapture capture;
    CuProcessExit status;
    CuProcessWait waited;
    char out[64];
    char err[64];
    char executable[] = "/bin/cat";
    char *args[2];
    int ok;
    int saved;
    int result;

    cu_arena_init(&arena);
    io = NULL;
    args[0] = executable;
    args[1] = NULL;
    capture.out = out;
    capture.out_cap = sizeof(out);
    capture.err = err;
    capture.err_cap = sizeof(err);
    if (!cu_process_io_start(&io, &arena, executable, args, NULL)) {
        perror("start");
        cu_arena_release(&arena);
        return 1;
    }
    child = cu_process_io_child(io);
    ok = cu_process_io_pump(io, cu_str_cstr("hello\n"), &capture, 1000);
    saved = errno;
    waited = cu_process_wait(child, ok ? 5000UL : 0UL, &status);
    if (waited == CU_PROCESS_ERROR)
        fatal("wait");
    if (waited == CU_PROCESS_RUNNING) {
        if (!cu_process_signal(child, CU_PROCESS_KILL) && errno != ESRCH)
            fatal("kill");
        do {
            waited = cu_process_wait(child, 1000, &status);
            if (waited == CU_PROCESS_ERROR)
                fatal("reap");
        } while (waited != CU_PROCESS_DONE);
    }
    result = ok && status.kind == CU_PROCESS_EXITED ? status.code : 1;
    if (!ok)
        fprintf(stderr, "pump failed, errno %d\n", saved);
    if (fwrite(out, 1, capture.out_len, stdout) != capture.out_len ||
        fwrite(err, 1, capture.err_len, stderr) != capture.err_len ||
        fflush(stdout) != 0 || fflush(stderr) != 0)
        fatal("output");
    cu_arena_release(&arena);
    return result;
}
```

Compile with the repository's C89 integration flags and `src/*.c`. `/bin/cat` is
an example executable path, not a portable location. On unsupported builds start
fails explicitly. Use stderr/stdout lengths even when their buffers contain NUL.

## Verification scope

Native tests execute a child that writes both outputs beyond their measured pipe
capacities before reading input larger than its pipe. Exact bytes, empty/exact/over
limits, caller signal state, pending SIGPIPE, live-child ownership, concurrent
sessions and initially closed parent stdin are checked. Synthetic boundaries inject
constructor and pump failures, partial/transient I/O, queued data with HUP, deadline
wrap/clamping, and unsafe-restoration guards. Misuse fixtures reject unexpected OS
calls with non-abort exits so fixture assertions cannot fake a guard pass.

A clean run is not a proof of all schedules, process isolation or all Linux kernel
configurations. No macOS/BSD or Windows communication backend is implemented here.
See the retained slice receipt for actual gate results and limits; existing fuzz
and formal harnesses cover their original modules, not a complete process model.
