# Synchronous bounded cooperative task batches

`cu_tasks_run(control, jobs, count, max_workers, timeout_ms)` runs an immutable
array of `CuTaskSpec` callbacks on at most `min(count, max_workers)` native
workers. Each spec contains `int (*fn)(CuTask *, void *)` and its context.
Success (1) means every job ran exactly once, returned nonzero, and every
worker joined. There is no serial fallback on unsupported platforms: nonempty
batches return 0 with ENOSYS. Empty batches succeed without allocation or OS
calls.

The initialized control arena, positive worker bound and positive timeout are
required even for an empty batch. NULL jobs are permitted only for count zero;
all nonempty specs must have a function. Invalid arguments abort before work
starts. Pointer/length validity remains the caller's responsibility.

## Complete C89 example

This prints `1 4 9 16` followed by a newline. Compile with the README's normal
command, using `src/*.c` and `-Iinclude`.

```c
#include <stdio.h>
#include "cu.h"

typedef struct Number {
    long input;
    long output;
} Number;

static int square(CuTask *task, void *context)
{
    Number *number;

    if (cu_task_cancelled(task))
        return 0;
    number = (Number *)context;
    number->output = number->input * number->input;
    return 1;
}

int main(void)
{
    CuArena control;
    CuTaskSpec jobs[4];
    Number numbers[4];
    size_t i;
    int result;

    cu_arena_init(&control);
    for (i = 0; i < 4; i++) {
        numbers[i].input = (long)i + 1;
        numbers[i].output = -1;
        jobs[i].fn = square;
        jobs[i].context = &numbers[i];
    }
    result = 1;
    if (!cu_tasks_run(&control, jobs, 4, 2, 2000))
        goto done;
    for (i = 0; i < 4; i++) {
        if (printf("%s%ld", i == 0 ? "" : " ", numbers[i].output) < 0)
            goto done;
    }
    if (putchar('\n') == EOF || fflush(stdout) != 0)
        goto done;
    result = 0;
done:
    /* All started workers have joined even when run failed. */
    cu_arena_release(&control);
    return result;
}
```

Each job writes a different element. The caller keeps both arrays alive until
run returns and releases control storage afterward. Arithmetic is bounded by
this example's fixed inputs, not a generic checked-square operation.

## Ownership and synchronization

The call borrows the array and contexts synchronously. Keep them alive and the
array immutable until return. Controller, worker records, mutex and native
thread records consume control-arena space, including on failure. No temporary
scope is internally ended around these records. Release their arena only after
return. Keep the control arena exclusive to this invocation, including its
scopes. Callbacks must not access its allocator state; use separate arenas for
their allocations. Shared user data needs caller-provided
synchronization.

No callback survives return: every successfully started worker is joined on
success, cancellation, timeout, and partial-start failure, then the mutex is
destroyed. Borrowed job-array references are cleared after joins. Job outputs
may be partial on failure; nothing rolls back callback effects.

The implementation passes direct regions checking without waivers. The run
function deliberately has no public regions borrowing summary: the current
checker reports contract drift for retained control-arena-internal record
references with a plain `borrows` summary, even with explicit clearing. This
means callers using scoped arguments may get conservative checker diagnostics;
it is not a promise that the checker proves the synchronous lifetime seam.
The callback accessors have borrowing summaries. Raw thread contracts are
unchanged.

## Stops, errors and budget

Callbacks return nonzero for success, zero for failure. errno is zeroed before
each invocation and captured immediately after a failure; zero is a valid saved
failure code. The first stop observed under the batch mutex wins, independently
of whether its saved error is zero. Failure returns 0 with that error, and
cleanup cannot replace it. Startup and clock failures also stop dispatch.

Within a callback only:

- `cu_task_cancel(task)` requests cancellation (ECANCELED unless already stopped).
- `cu_task_cancelled(task)` checks any failure, cancellation or deadline.
- `cu_task_remaining(task, &ms)` returns 1 with positive remaining milliseconds,
  rounded up, or 0 with zero milliseconds and the saved error.

Never retain a task pointer or use it outside its callback. Callbacks must return
normally; do not terminate/cancel worker threads or jump out of a callback.
Accessors synchronize all stop/deadline state through the batch mutex. One CLOCK_MONOTONIC budget spans
startup and all callbacks; it is checked before dispatch, by accessors, after
callbacks, and before success. Deadline expiry saves ETIMEDOUT. Unsigned elapsed
arithmetic avoids signed absolute-deadline overflow, including ULONG_MAX budgets.

Pending jobs may be skipped after a stop; callbacks already dispatched may run.
Cancellation is cooperative: poll accessors and bound blocking operations using
the remaining budget. A callback that ignores cancellation or blocks forever can
delay the batch forever. This is not a hard return-time guarantee, persistent
executor, external cancellation interface, or general structured-concurrency
runtime.
