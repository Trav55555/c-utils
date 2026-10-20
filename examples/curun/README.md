# curun

A small Linux command runner built against c-utils' public process APIs. It
captures bounded binary stdout/stderr and explicitly reaps its direct child.
There is no shell, PATH search, terminal emulation or process-tree supervision.
No new library API is required.

## Build and use

From the repository root, with a C89 compiler:

```sh
make curun
build/curun --timeout-ms 1000 --report -- /usr/bin/git status --short
build/curun --stdout-limit 4096 --stderr-limit 4096 -- /bin/echo 'literal argument'
make test-curun
```

The build uses the repository's strict C89 flags. No checker toolchain is needed
for these commands. `CC`, `CFLAGS` and `BUILD` can select an alternative compiler,
instrumented build and output directory. The binary is `BUILD/curun`. Use separate
BUILD directories when changing compiler/flags, or use make's `-B` option to
rebuild existing outputs.

Syntax:

```text
curun [--timeout-ms N] [--stdout-limit N] [--stderr-limit N]
      [--report] -- EXECUTABLE [ARG ...]
```

- The command separator is mandatory. The executable must contain `/`, such as
  `/usr/bin/git` or `./program`. Arguments are passed literally, including empty
  strings, whitespace and shell metacharacters.
- Timeout defaults to 5,000 ms. It must be a positive decimal unsigned-long value.
  Zero does not mean unlimited. Signs, whitespace, trailing text and overflow fail.
- Capture defaults to 1,048,576 bytes per stream. Each limit is independently
  configurable from zero through 16,777,216 bytes. A zero limit accepts empty
  output only. Exact capacity is accepted; one extra byte fails without silent
  success or retained overflow. Repeated options use the last value.
- The child inherits the environment and working directory. Its stdin immediately
  receives EOF. This version does not read parent stdin or accept an input file.
  Catchable child signals/masks use the library's default-spawn policy. Other
  non-CLOEXEC descriptors can inherit; this is not an FD whitelist or sandbox.

## Output and exit codes

Captured prefixes are forwarded after the child has been reaped, including on
communication failure. Output is binary, not NUL-terminated or decoded. Relative
ordering between the two streams is not preserved. A failed exchange may leave
unread bytes on the other stream, so prefixes are not a transcript of all child
output. Wrapper diagnostics use stderr and may precede the captured child stderr.
Capture limits apply to child data, not wrapper diagnostics or `--report` lines.

Normal child output is otherwise unchanged. `--report` appends either
`curun: child exited N` or `curun: child signal N` to stderr after the captured
prefix. It does not insert a newline before that line if child stderr lacked one.
Raw child output can contain terminal control sequences; select trusted commands.

| Result | Exit code |
|---|---:|
| Normally exited child | Child exit code |
| Signaled child | 128 plus native signal number, capped at 255 |
| Invalid CLI arguments | 2 |
| Execution deadline | 124 |
| Capture overflow, allocation/start/I/O/clock/output failure | 125 |
| Wrapper cancellation | 128 plus received signal number, capped at 255 |

Child exits 2, 124 or 125 are still passed through. `--report` and wrapper error
messages distinguish those from wrapper failures. Start success cannot distinguish
a genuine child exit 127 from an exec/setup failure reported that way by POSIX.
No signal is sent after an observed DONE status.

## Deadline, cancellation and cleanup

One monotonic execution budget starts after successful spawn, before communication.
The remainder, not a new full duration, covers exit waiting after stream EOF.
A child that closes both streams and keeps running therefore still times out.

On timeout, capture failure or cancellation, curun closes communication endpoints,
requests SIGKILL for the direct child, and polls until the library reports DONE.
Every ordinary controlled return retains its arena until that point. Cleanup uses
clock-independent wait polls so a clock error cannot discard a live handle.
Persistent wait/kill failure can make cleanup wait indefinitely rather than drop
ownership. There is no hard return bound for spawn, OS cleanup, scheduling, or
forwarding captured bytes to a blocked destination. External SIGKILL, process
crashes and machine failure cannot provide a reaping guarantee.

SIGINT, SIGTERM and normally handled SIGHUP record cancellation without jumping
through the library. An inherited SIGHUP ignore remains ignored in the wrapper.
During exit waiting cancellation is checked at most every 50 ms. During the
one-shot communication pump it is deferred until pump completion or its configured
deadline; the existing API has no external cancellation parameter. Thus a long
`--timeout-ms` can also mean long cancellation latency. Cleanup itself is not
hard-deadline-bounded. The child's signal defaults follow `CuProcess` policy,
not the wrapper's temporary handlers or ignored SIGPIPE disposition.

Only one direct child is owned. Descendants are not killed or reaped by curun;
a descendant holding a pipe writer can keep communication open until the deadline.
No CPU, child-memory, credential, filesystem or tenant isolation is provided.
Use an external supervisor/cgroup for process-tree containment. Do not use curun
as a hostile-code executor.

## Verification and API feedback

The C tests exercise CLI parsing, real child statuses and failures, clock-budget
boundaries, and injected allocation/start/clock/pipe/wait/kill/handler/output
failures. The native Python tests check exact bytes and literal arguments,
inherited environment, simultaneous output larger than pipe capacity, stdin EOF,
zero/exact/over limits, early stream closure, cancellation, output failures and
actual direct-child reaping with a subreaper oracle. Numeric parsing is fuzzed
against an independent decimal-length/lexicographic range oracle.

Maintainer checks require the separate toolchain. In this directory, `make check`
runs the consumer's own gate with coverage floor 90 and regions enabled, including
all library sources through DEPS. `make deep` adds its configured semantics,
Valgrind and 10-second parser fuzz run under the maintained 1,800-second/3GiB/no-swap
runner. There is no new formal proof for this CLI or process lifecycle; the
library's existing proof/fuzz scopes are unchanged. Unsupported builds return 125
for command execution; native non-Linux platforms have not been validated.

This consumer exposes three API costs without changing the library: composing a
communication/exit deadline needs a small caller clock adapter, one-shot pumping
cannot provide prompt external cancellation, and explicit endpoint closure plus
reaping remains caller policy. Those are feedback for future API decisions, not
new guarantees of this CLI.
