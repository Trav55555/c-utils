/* The malformed-request cases of test_http.c, in a file of their own: under
 * Cerberus a run slows as it goes, and the whole file took 352 s of dcheck's
 * 300 s per-file limit on a loaded host, these cases 106 s alone. The
 * rest of test_http.c, renamed out of the way, supplies the wire oracles
 * and probe(). */
int test_http_main(void);
#define main test_http_main
#include "test_http.c"
#undef main

/* Cerberus scans a head byte by byte at about 30 ms a byte, and the full
 * tables below ran it for 219 s of dcheck's 300 s per-file limit. Under it
 * the entries that repeat a path another entry takes are left out (marked
 * "native only"); every branch of cu_http_begin that rejects or sets aside
 * a head is still reached by an entry that stays. Native, sanitizer and
 * coverage builds run every entry. */
static void malformed(void)
{
    static const char *bad[] = {
        "Host: x\r\nContent-Length: 1\r\nContent-Length: 1",
#ifndef __cerb__
        "Host: x\r\nContent-Length: 1\r\nContent-Length: 2",
        "Host: x\r\nContent-Length: 1,1",
#endif
        "Host: x\r\nContent-Length:",
        "Host: x\r\nContent-Length: +1",
#ifndef __cerb__
        "Host: x\r\nContent-Length: 1 0",
#endif
        "Host: x\r\nContent-Length: 99999999999999999999999999999999",
        "Host : x", "Host: x\r\n folded: x", "Host: x\nX: y",
        "Host: x\rX: y", "Host:", "Host: x\r\nhost: y", "X: y",
        "Host: x\r\n@: y", "Host: x\r\nX: \177",
#ifndef __cerb__
        "Host: x\r\nX: \200",
#endif
        /* RFC 9112 6.3: Transfer-Encoding lines form one list, and a list
         * whose final coding is not chunked, or that has no coding, is a
         * 400. Chunked must not precede another coding (6.1), and TE with
         * Content-Length is rejected as an error (6.1, 6.3). */
        "Host: x\r\nTransfer-Encoding: gzip",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: identity",
        "Host: x\r\nTransfer-Encoding: nonsense",
#endif
        "Host: x\r\nTransfer-Encoding:",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: , ,",
#endif
        "Host: x\r\nTransfer-Encoding: chunked, chunked",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: chunked, gzip",
        "Host: x\r\nTransfer-Encoding: chunked;x=1",
        "Host: x\r\nTransfer-Encoding: chunke",
#endif
        "Host: x\r\nTransfer-Encoding: chunkedx",
        "Host: x\r\nTransfer-Encoding: chun ked",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip",
        "Host: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked",
        "Host: x\r\nTransfer-Encoding:\r\nTransfer-Encoding: ,",
        "Host: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1",
#endif
        "Host: x\r\nContent-Length: 1\r\ntRaNsFeR-EnCoDiNg: chunked"
#ifndef __cerb__
        ,
        "Host: x\r\nContent-Length: 1\r\ntRaNsFeR-EnCoDiNg: identity",
        "Host: x\r\nExpect: 100-continue\r\nTransfer-Encoding: gzip",
        "Host: x\r\nTransfer-Encoding: gzip\r\nUpgrade: h2c"
#endif
    };
    static const char *unsupported[] = {
        "Host: x\r\nTransfer-Encoding: chunked",
        "Host: x\r\ntRaNsFeR-EnCoDiNg: ChUnKeD",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: gzip, chunked",
        "Host: x\r\nTransfer-Encoding: chunke, chunked",
#endif
        "Host: x\r\nTransfer-Encoding:  ,chunked,\t, \t",
        /* The transfer-coding list continues across field lines. */
        "Host: x\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked",
#ifndef __cerb__
        "Host: x\r\nTransfer-Encoding: gzip , chunked \t,",
        "Host: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: ,",
        "Host: x\r\nTransfer-Encoding:\r\nTransfer-Encoding: chunked",
#endif
        "Host: x\r\nExpect: 100-continue", "Host: x\r\nUpgrade: h2c"
    };
    static const char *lines[] = {
        " GET / HTTP/1.1", "G@T / HTTP/1.1", "GET * HTTP/1.1",
        "GET /#x HTTP/1.1", "GET / HTTP/1.0", "GET / x HTTP/1.1",
        "GET /\t HTTP/1.1"
#ifndef __cerb__
        , "GET /\200 HTTP/1.1", "\r\nGET / HTTP/1.1"
#endif
    };
    char wire[512];
    size_t i;
    for (i = 0; i < sizeof bad / sizeof *bad; i++) {
        sprintf(wire, "GET / HTTP/1.1\r\n%s\r\n\r\n", bad[i]);
        probe(wire, strlen(wire), sizeof wire, 20, (size_t)-1, CU_HTTP_BAD_REQUEST);
    }
    for (i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
        sprintf(wire, "GET / HTTP/1.1\r\n%s\r\n\r\n", unsupported[i]);
        probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_UNSUPPORTED);
    }
    for (i = 0; i < sizeof lines / sizeof *lines; i++) {
        sprintf(wire, "%s\r\nHost: x\r\n\r\n", lines[i]);
        probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_BAD_REQUEST);
    }
    probe("CONNECT / HTTP/1.1\r\nHost: x\r\n\r\n", 31, 512, 20, 20, CU_HTTP_UNSUPPORTED);
    /* A malformed Transfer-Encoding outranks a body over the limit. */
    strcpy(wire, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 21\r\n\r\n");
    probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_LIMIT);
    strcpy(wire, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 21\r\n"
                 "Transfer-Encoding: gzip\r\n\r\n");
    probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_BAD_REQUEST);
    memcpy(wire, second, sizeof second);
    wire[5] = 0;
    probe(wire, sizeof second - 1, 512, 20, 20, CU_HTTP_BAD_REQUEST);
}

int main(void)
{
    malformed();
    return 0;
}
