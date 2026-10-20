# cuhealth: finite local health/status consumer

Maintained example of application policy above c-utils' request reader,
response-head serializer, poller, thread, mutex and SHA256 APIs. Linux/POSIX
foreground example, not a deployment recipe or a complete HTTP server.
Unsupported socket/thread/clock backends fail startup (including WASI and
Cerberus); there is no pretend serial/server fallback.

```
make
./build/default/cuhealth --port 0 --requests 8 --timeout-ms 1000 --rounds 1000
make test
make check
make deep
```

Use separate `BUILD=build/gcc`, `BUILD=build/clang`, etc. for different compiler
flags. `main.c` is outside `src` so dcheck can link its own test mains.
The consumer dcheck configuration retains 90% coverage, ten-second fuzzing,
and regions checking; no additional waiver or check reduction is requested.
Gate/deep run in the repository's 3 GiB/no-swap bounded verification scope,
with aggregate 600/900 second limits. A cap or optional skip is not all-pass.
Toolchain qualification target: profile `656f4076c75c5f19488a`.

## Lifecycle and wire policy

The first stdout line is `127.0.0.1:PORT`, flushed after binding. Only IPv4
loopback is bound. Default port zero asks the OS for a port. Defaults are eight
accepted connections, a 1000 ms idle/read/send budget, and 1000 SHA256 rounds.
`--port` accepts 0..65535; `--requests` and `--timeout-ms` accept positive
unsigned-long decimal values; `--rounds` accepts 1..1000000. No signs or suffixes.

A single arena-owned background thread repeatedly checks SHA256(`abc`) against
the known answer. The mutex protects checking/ready/failed and cooperative stop.
The worker checks stop each iteration and never calls datetime functions.
It is joined before destroying its mutex or releasing its arena, even when
startup fails after thread creation. Cancellation is not preemptive.

One connection is handled at a time. Each accepted connection counts toward
the finite limit even if its peer fails. Idle listener expiry can end sooner.
The listener is watched one-shot, and only accessed again after readiness or
poller destruction. Poll waits are at most 25 ms to notice SIGINT, SIGTERM or
SIGHUP; handlers only set a non-raising signal flag. Active request work can
delay noticing that flag until its current budgets finish. Normal exit requests
worker stop and joins it. External SIGKILL cannot guarantee cleanup.

Routes are fixed exact byte strings, with no URL decoding or normalization:

* GET/HEAD `/health`: 200, `ok\n` (process availability, not job readiness).
* GET/HEAD `/status`: JSON `{"status":"checking"}` (503), `ready` (200),
  or `failed` (503), followed by newline.
* Other paths: 404. Other supported methods: 405 with `Allow: GET, HEAD`.
* Invalid or excluded framing: attempt 400 then close. Sticky I/O errors such
  as reader timeout may prevent a response; closure remains mandatory.

The strict request head is at most 2048 bytes, with at most 16 fields and a
zero-byte body limit. Only successful zero bodies are consumed. Rejected input
is never drained; pipelined requests are never processed. Closing with unread
peer bytes can produce a TCP reset instead of an orderly EOF.

The new head-only serializer owns Connection and Content-Length. The consumer
passes only Date, Content-Type and, for 405, Allow views. HEAD advertises the
selected GET representation length but sends no body. The head and finite body
are assembled into one bounded buffer and sent with **one** socket-write call.
Date is IMF-fixdate UTC via main-thread-only time/gmtime/strftime in the initial
C locale; the application never calls setlocale.

## Trust and bounds

Loopback is not a security boundary. Other local processes can connect, consume
the finite quota, hold the serial connection until its timeout, or observe the
status. There is no authentication, TLS, daemon, deployment, service management,
shell, remote command, subprocess job, arbitrary path or user-selected job.
Only the predefined startup SHA256 check runs.

One positive reader budget covers all head acquisition and successful empty
body completion; a **separate** positive send budget covers the single write.
The idle listener budget is separate again. These do not constitute a single
whole-request wall-clock deadline: OS scheduling, cleanup and thread joining
are not hard-real-time bounded. Positive unsigned-long inputs can intentionally
select very large finite budgets. This is a correctness-first example, not a
throughput-tested public server or a protocol/security certification.

## Tests

C tests inject policy state (not public CLI flags) to cover deterministic
checking/ready/failed responses, HEAD lengths, routes, parser integration,
reader timeout, SHA answer, cooperative stop and startup failures. Python
socket tests use raw requests and `http.client.HTTPResponse`, verify dates,
Content-Length, actual HEAD boundaries, JSON, fixed paths, malformed/framing
limits, closure, finite accepted count, idle exit and all three stop signals.
Test subprocess creation belongs only to the test harness, never the consumer.
