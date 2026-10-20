# Numeric TCP connect with a deadline

```c
int cu_socket_connect_ip_for(CuSocket *socket, const char *ip, unsigned port,
                             unsigned long timeout_ms);
```

This operation connects to one numeric IPv4 or IPv6 literal without invoking
`getaddrinfo`, NSS or DNS. A positive timeout is one monotonic opening budget.
Zero disables that connection deadline. The existing hostname-taking
`cu_socket_connect` remains unbounded; setting its later I/O timeout does not
retroactively bound name resolution or connection establishment.

Use the numeric operation after an application has chosen an address. A hostname
API with bounded DNS needs a separate asynchronous resolver contract; hiding a
blocking resolver behind a connect timeout would not provide that guarantee.
There is no address-list racing, Happy Eyeballs or TLS in this slice.

## Address and ownership

`ip` is a valid NUL-terminated string, borrowed only during the call. Ordinary
`inet_pton` IPv4/IPv6 syntax is accepted. Hostnames, empty or overlong strings,
brackets such as `[::1]` and zone identifiers such as `fe80::1%eth0` fail with
`EINVAL`. Inspection is bounded to `INET6_ADDRSTRLEN`; a maximum 45-character
literal remains valid. Ports above 65535 fail with `EINVAL`; port zero follows
native connect behavior, not an invented service rule.

NULL socket/address pointers abort. As with existing socket opening calls, the
output can be uninitialized: close any previous ownership before opening into it.
Opening over a live socket violates that ownership rule. Close the initialized
result even when opening fails.

Return 1 publishes one blocking, close-on-exec TCP connection with the existing
no-SIGPIPE write configuration. Its later read/write timeout is still zero. Call
`cu_socket_set_timeout`, `cu_socket_read_for` or a timed reader for later I/O.
Connection and exchange budgets are separate, not an implicit end-to-end deadline.

Return 0 retains a sticky failure and errno, including zero, and closes each new
temporary descriptor once. Retrying an interrupted close could close a reused
number. Linux consumes those descriptors; other POSIX close/EINTR rules can be
ambiguous, so complete reclamation there is not guaranteed. The failed object's FD
is -1. Read/write and repeated close
replay that saved failure. Cleanup never replaces the operation's error. Socket
close now explicitly restores saved errno on repeated failed closes, including the
unsupported backend, rather than relying on the caller leaving errno untouched.

There is no new regions summary: the checker does not model `inet_pton` borrowing.
The runtime borrowing contract remains; passing the other regions checks is not a
static proof of this native function's pointer lifetime or all platforms.

## Budget and completion

The positive budget starts before parsing/setup and spans all connection attempts,
polls, interruptions, completion queries and restoration of blocking flags. It is
never restarted on progress or EINTR. Remaining durations use unsigned subtraction,
not signed absolute deadline addition; long poll intervals clamp to INT_MAX.
A successful operation cannot publish a connection observed past expiry.

A nonblocking attempt returning EINPROGRESS or EALREADY is polled for writable
completion. POLLERR/HUP also require SO_ERROR; readiness alone is not success.
The native SO_ERROR result, not ambient errno, determines connection failure.
Unrelated/spurious readiness is not completion. Bare HUP/ERR with zero SO_ERROR
and no writable indication fails with ECONNABORTED; invalid descriptors give EBADF.
Malformed native completion output fails rather than looking like zero success.

POSIX leaves the state of an interrupted initial connect unspecified. For EINTR,
the implementation closes that attempt and retries a fresh socket under the
original budget. It never repeatedly calls connect on an in-progress descriptor.
A retry can establish and abandon a connection, so a peer might observe an early
connection that receives no application bytes. Ordinary refusal or resource errors
such as EAGAIN propagate without being reclassified as connection progress.
Interrupted polls retry the same pending attempt. Failed completion queries and
clock/setup errors propagate, including zero errno.

Timeout gives ETIMEDOUT. An independently observed native failure can win a race
with expiry; there is no stronger cross-event ordering promise. Scheduler, native
syscall and cleanup delays are not hard real-time bounded. No thread cancellation
or non-local jump through resource cleanup is supported.

On Linux, descriptor creation is atomically CLOEXEC and nonblocking. Other existing
POSIX socket backends use their documented fcntl close-on-exec fallback, with an
inheritance window under concurrent exec. Do not treat this as a new atomicity or
sandbox guarantee there. Blocking mode is restored only on the private descriptor
before publication, preserving other status bits. Unsupported socket backends
return ENOSYS and leave a failed object safe to close. Native Apple/BSD/Windows
runtime support is not established by Linux or WASI checks.

## Complete C89 example

The example connects to a caller-selected literal and port, then closes it. A
successful TCP connection is not application readiness, authentication or TLS.
There is no DNS fallback. The message goes to stdout only after clean close.

```c
#include <errno.h>
#include <stdio.h>
#include "cu.h"

int main(int argc, char **argv)
{
    CuSocket socket;
    unsigned long port;
    int connected;
    int saved;
    int closed;

    if (argc != 3 || !cu_parse_ulong(cu_str_cstr(argv[2]), 10, &port) || port > 65535) {
        fputs("usage: connect-example IP PORT\n", stderr);
        return 2;
    }
    connected = cu_socket_connect_ip_for(&socket, argv[1], (unsigned)port, 1000);
    saved = errno;
    closed = cu_socket_close(&socket);
    if (!connected) {
        fprintf(stderr, "connect failed, errno %d\n", saved);
        return 1;
    }
    if (!closed) {
        fprintf(stderr, "close failed, errno %d\n", cu_socket_errno(&socket));
        return 1;
    }
    if (fputs("connected\n", stdout) == EOF || fflush(stdout) != 0)
        return 1;
    return 0;
}
```

Compile with the repository's C89 flags and `src/*.c`. Use an actual local server
or a destination that the caller is authorized to contact. The example does not
create a listener, modify networking policy or reserve external infrastructure.

## Verification scope

Native fixtures use independent kernel listeners and exact bidirectional binary
bytes. They check blocking/CLOEXEC flags, timeout independence, invalid input,
refusal while a bound non-listener reserves the port, and descriptor cleanup.
IPv6 is exercised only after loopback capability succeeds; a recognized capability
failure is an explicit skip, never permission to skip a failed connection after
listening worked.

A deterministic boundary fixture verifies no resolver calls, sockaddr byte/port
layout, interrupted attempts and polls, SO_ERROR failures, spurious/zero readiness,
late success rejection, first-error cleanup including zero, and large/wrapped time
arithmetic. Its guard tests reject unexpected OS calls by non-abort exit, so fixture
assertions cannot counterfeit an expected SIGABRT. HTTP framing/task loopback
clients use the new path; the unchanged legacy socket suite still exercises the
hostname API. See the retained slice receipt for actual profiles and results.

These are scoped functional checks, not a network security audit, race-freedom
proof, TLS implementation, universal portability claim or model of all TCP states.
