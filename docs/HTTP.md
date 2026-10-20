# Bounded HTTP request bodies

`CuReader` and `CuHttpRequest` provide a first request-framing slice for larger C libraries. They do not provide a complete HTTP/1.1 server, routing, TLS, or authentication.
A separate [restricted response-head serializer](HTTP_RESPONSES.md) supplies
bounded framing bytes; sending bodies and closing connections remain caller policy.

The parser accepts a deliberately restricted profile. Do not put it in front of another parser as a validating HTTP proxy, or assume that it validates a Host authority or decodes a URL.

## Read a body without buffering it all

This complete C89 program prints `hello` without a trailing newline:

```c
#include <stdio.h>
#include "cu.h"

int main(void)
{
    static const char wire[] =
        "POST /upload HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Content-Length: 5\r\n\r\nhello";
    CuReader reader;
    CuHttpRequest request;
    CuHttpResult result;
    char head[256];
    char body[4];
    size_t n;

    cu_reader_memory(&reader, cu_str_n(wire, sizeof wire - 1));
    result = cu_http_begin(&request, &reader, head, sizeof head, 16, 1024);
    if (result != CU_HTTP_OK)
        return 1;
    while ((result = cu_http_read(&request, body, sizeof body, &n)) == CU_HTTP_OK) {
        if (fwrite(body, 1, n, stdout) != n)
            return 1;
    }
    if (result != CU_HTTP_END || fflush(stdout) != 0)
        return 1;
    return 0;
}
```

Build it using the README's normal command with `src/*.c` and `-Iinclude`.
The head buffer bounds the entire request head, including the final CRLFCRLF.
The last two begin arguments bound field count and body length. Both may be
zero: zero fields cannot accommodate the required Host, and zero body length
allows requests without a body.

Method, target, and head are borrowed views into `head`. Keep that storage
alive and do not overwrite it while using those views. Body bytes can contain
anything, including NUL. Consume or discard each body chunk before reusing its
buffer. Body reads return at most the remaining Content-Length, never a byte of
the next request. The final data chunk returns OK; the following read returns
END with zero bytes. `request.remaining == 0` also identifies a complete body.

## Use an existing socket

After accepting a connection into an initialized `CuSocket socket`, bind a
reader with a positive millisecond budget:

```c
if (!cu_reader_socket(&reader, &socket, 5000)) {
    /* Handle cu_reader_errno(&reader), then close socket. */
}
```

Only call begin after binding succeeds. One monotonic budget covers the whole
reader lifetime, including head acquisition, body reads, interruptions, and
caller pauses. A peer cannot reset it by delivering another byte. This is a
millisecond-resolution blocking-I/O budget, not a hard real-time scheduling
guarantee. It does not bound DNS lookup, connection establishment, response
writing, or total application CPU time.

The reader borrows the socket. It does not change the socket's configured
per-call timeout, its poller membership, or ownership. The socket must not be
listening or watched. Serialize access and do not use the socket outside its
reader while borrowed. Close the socket explicitly and check close's aggregate
error result.

After completely consuming a successful request, abandon its views and bind a
new reader if the next request should have a fresh budget. There is no hidden
read-ahead buffer, so the next request remains in the source. Memory readers
have no clock budget and can parse consecutive requests on the same cursor.

`cu_socket_read_for` is the underlying per-operation budget override. It keeps
the configured timeout unchanged and retains ordinary socket sticky-error
behavior. Stdio inputs are not adapted into deadline-aware readers because a
blocking file or pipe read cannot honor this same contract portably.

## Accepted framing and rejection policy

The profile follows the basic syntax and body-length rules of
[RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html), with explicit restrictions:

- HTTP/1.1 only, a token method, and an origin-form target starting with `/`.
  No leading blank lines, alternative whitespace separators, fragments, or
  non-ASCII head bytes are accepted. The target is not decoded or normalized.
- Exactly one nonempty Host field. Its authority syntax is not validated.
  Treat it as untrusted input if later code uses it for routing or URLs.
- No Content-Length means an empty request body, including for POST.
- At most one Content-Length field, containing decimal digits after optional
  surrounding spaces/tabs. Leading zeroes are accepted. Signs, comma lists,
  internal whitespace, unrepresentable values, and duplicates are rejected.
  Equal duplicate lengths are rejected too, rather than normalized.
- Every Transfer-Encoding is unsupported, including chunked and identity.
  A request containing both Transfer-Encoding and Content-Length is never
  accepted. Expect, Upgrade, and the CONNECT method are unsupported too.
- Field names must be tokens without whitespace before the colon. Bare LF,
  bare CR, folded fields, NUL, DEL, and other control bytes are rejected.
  Tabs are allowed in field values, not as request-line separators.

`CU_HTTP_LIMIT` reports configured head, field-count, or representable body-size
limits. `CU_HTTP_BAD_REQUEST` reports malformed supported syntax, including a
decimal length that cannot fit size_t. `CU_HTTP_UNSUPPORTED` reports excluded
protocol features. `CU_HTTP_TRUNCATED` reports early EOF. A clean EOF before
any head bytes returns END. Metadata is empty on a failed begin, although the
caller's head buffer may have been partially written.

`CU_HTTP_IO` preserves the reader's first errno, including ETIMEDOUT and zero.
Use the status to recognize failure; errno zero does not mean success. Errors
are sticky, publish no bytes for that call, and suppress further reads. An I/O
failure can consume bytes that it does not publish. Previously returned body
chunks cannot be rolled back.

After any framing, limit, timeout, or I/O error, discard the connection. Do not
try to locate the next request by draining a rejected request. The parser
does not automatically generate an HTTP error response or enforce connection
closure. A caller may use the separate response-head serializer for a restricted
error reply, then must discard the connection.

## Scope and verification

Head acquisition currently reads one byte per call. That preserves the actual
underlying source cursor but costs one socket read per head byte. Body reads
use caller-sized chunks. This implementation is a correctness-first foundation,
not a throughput-tested server parser.

Tracked tests exercise strict syntax, size boundaries, binary and pipelined
bodies, truncated input, first-error behavior, overlapping memory reads, and
real TCP deadlines. `fuzz/fuzz_http.c` checks bounded parser/accounting
invariants under arbitrary memory input. `proofs/proof_http_body.c` models
0..4 body bytes and 1..2-byte buffers with a following sentinel frame. That
proof does not cover head parsing, sockets, clocks, or all inputs.

Run `make check` and `make deep` to verify your checkout. The coordinated deep
check for this framing slice passed. Verification covers the supported profile
and the stated model bounds, not arbitrary HTTP implementations. No external
safety, CERT C, MISRA, or protocol-conformance certification is claimed.
