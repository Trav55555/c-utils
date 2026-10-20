# Restricted HTTP/1.1 response heads

`cu_http_response_head` serializes a complete, bounded HTTP/1.1 head into caller
storage. It does not allocate, send bytes, retain views, alter errno or write a
body. It is separate from request parsing and transport/application policy.

```c
#include <stdio.h>
#include "cu.h"

int main(void)
{
    CuHttpHeader field;
    char head[256];
    size_t n;
    field.name = CU_LIT("Content-Type");
    field.value = CU_LIT("text/plain");
    if (cu_http_response_head(head, sizeof head, 200, 3, &field, 1, &n)
        != CU_HTTP_RESPONSE_OK)
        return 1;
    if (fwrite(head, 1, n, stdout) != n || fwrite("ok\n", 1, 3, stdout) != 3 ||
        fflush(stdout) != 0)
        return 1;
    return 0;
}
```

This emits `HTTP/1.1 200 OK`, `Connection: close`, `Content-Length: 3`, the
Content-Type field and a final empty line, then the example's three body bytes.
Use the normal C89 build with `src/*.c` and `-Iinclude`. The finite
[cuhealth consumer](../examples/cuhealth/README.md) demonstrates actual sockets,
Date generation, HEAD, fixed routes and a cooperative startup-check job.

## Result and storage contract

Compare the enum explicitly: OK is 1, while INVALID and LIMIT are negative.
On success `*written` is the exact byte count, at most cap, with no trailing NUL.
On any ordinary failure `*written` is zero and every output byte is unchanged.
LIMIT includes insufficient capacity and checked size arithmetic overflow;
INVALID means excluded status/framing/field syntax. A zero capacity with NULL
head is a valid failing size-limited call, not a size-query API.

The header vector and all `CuStr` views are borrowed only during the call.
They must remain valid and stable, and must not overlap head or written.
`headers == NULL` is allowed only for count zero; `head == NULL` only for capacity
zero. Required NULL-pointer misuse aborts. As elsewhere in C, arbitrary invalid
addresses or length/storage mismatches cannot be detected portably.

## Deliberately narrow wire profile

- Status codes 200..599 are accepted. Informational responses are excluded.
  Common codes receive fixed reason phrases; unknown codes use an empty phrase,
  with the required separating space still present.
- Every head contains `Connection: close`. Except for 204/304, it contains exactly
  one generated decimal Content-Length, representing body bytes, not characters.
- 204/205/304 require body_length zero. 204/304 omit Content-Length entirely;
  205 emits Content-Length: 0. A 304 zero is a sentinel for this restricted API,
  not a claim about the selected 200 representation's length.
- Header names are nonempty ASCII tokens. Values may be empty, or visible ASCII
  with internal spaces/tabs. Edge whitespace, CR/LF/NUL, other controls, DEL and
  non-ASCII bytes are rejected. This is stricter than HTTP's obs-text allowance.
- Caller Content-Length, Transfer-Encoding, Connection, Trailer, Upgrade, TE,
  Keep-Alive and Proxy-Connection fields are rejected case-insensitively. The
  literal TE field is distinct from Transfer-Encoding; both are excluded here.
  Other fields retain caller spelling/order and can repeat. Their individual
  grammar and duplicate-field semantics are not validated.

All syntax/size checks precede output emission. Time is linear in the supplied
head bytes and field count; memory use is constant apart from caller storage.
No body bytes are inspected, copied or used to derive a length automatically.

## Consumer obligations

For ordinary responses send exactly the advertised number of body bytes, then
close. Connection: close does not override Content-Length framing. Do not process
another request on that connection. Sending fewer/more bytes, an unsupported
transfer coding, or appended trailers is not made safe by this constructor.

For HEAD, supply the corresponding GET representation length but send no body.
Do not send content for 204/205/304. Successful CONNECT responses are outside the
profile: never use the ordinary generated Content-Length for a tunnel. The
restricted request parser already excludes CONNECT, but callers using another
request source must enforce that rule themselves.

The caller supplies field-specific and status-required metadata, including Date
when required by the origin-server clock rules, and correct Location/Allow/etc.
This helper does not enforce caching semantics or metadata requirements for
statuses such as 304. It is not an HTTP proxy validator or a complete/conforming
HTTP/1.1 server. The example's routing, self-check, Date formatting and listener
policy are consumer code, not library mechanisms.

Transport deadlines are also separate. Do not inadvertently reset one logical
response budget by repeatedly calling independently timed writes. cuhealth
combines its small head/body into one bounded buffer for one socket-write call;
its read and send budgets are explicitly separate. TLS, authentication, public
exposure, streaming/chunked bodies, keep-alive, compression and process/tenant
isolation are not implemented by this slice.

## Evidence and standards

Literal wire tests and field/capacity canaries exercise valid framing, no-content
statuses, maximum size_t lengths, reserved fields, header injection and failure
atomicity. A memory fuzzer checks framing/accounting under bounded arbitrary
input. Socket/service tests use independent raw wire expectations and Python's
HTTP parser. Existing request-body proofs retain their old scope; there is no
new formal proof of the complete serializer, service, clocks or network behavior.
See the task receipt for actual checks and failures, not a universal safety claim.

Primary requirements:

- [RFC 9112 section 4](https://www.rfc-editor.org/rfc/rfc9112.html#section-4):
  status-line grammar, including the space before an absent reason phrase.
- [RFC 9110 section 8.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6):
  Content-Length restrictions for 204, HEAD, 304 and successful CONNECT.
- [RFC 9110 section 15.3.6](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.3.6):
 205 cannot contain content; our explicit zero length is a framing choice.
- [RFC 9110 section 5.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.5):
  field values, with dangerous CR/LF/NUL excluded.
- [RFC 9112 sections 6.3 and 9.6](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3):
  body framing and the caller's close requirement.
