#include "cuhealth.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) && !defined(__wasi__) && !defined(__cerb__)
#include <signal.h>
static volatile sig_atomic_t interrupted;
static void health_signal(int signo) { (void)signo; interrupted = 1; }
#else
static int interrupted;
#endif

int health_number(const char *text, unsigned long max, unsigned long *out)
{
    unsigned long n;
    unsigned long digit;
    n = 0;
    if (*text == '\0') return 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') return 0;
        digit = (unsigned long)(*text - '0');
        if (digit > max || n > (max - digit) / 10) return 0;
        n = n * 10 + digit;
        ++text;
    }
    *out = n;
    return 1;
}

void health_worker(void *context)
{
    static const unsigned char answer[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    HealthJob *job;
    unsigned long i;
    unsigned char digest[32];
    int stop;
    job = (HealthJob *)context;
    for (i = 0; i < job->rounds; ++i) {
        cu_mutex_lock(job->mutex);
        stop = job->stop;
        cu_mutex_unlock(job->mutex);
        if (stop) return;
        cu_sha256(cu_str_cstr("abc"), digest);
        if (memcmp(digest, answer, sizeof answer) != 0) {
            cu_mutex_lock(job->mutex);
            job->state = -1;
            cu_mutex_unlock(job->mutex);
            return;
        }
    }
    cu_mutex_lock(job->mutex);
    job->state = 1;
    cu_mutex_unlock(job->mutex);
}

int health_snapshot(HealthJob *job)
{
    int state;
    cu_mutex_lock(job->mutex);
    state = job->state;
    cu_mutex_unlock(job->mutex);
    return state;
}

static int equal(CuStr view, const char *text)
{
    return view.len == strlen(text) && memcmp(view.ptr, text, view.len) == 0;
}

int health_wire(char *wire, size_t cap, size_t *written, CuStr method,
                CuStr target, int valid, int state, const char *date)
{
    CuHttpHeader headers[3];
    unsigned status;
    const char *body;
    const char *type;
    size_t count;
    size_t head;
    size_t length;
    status = 200;
    body = "ok\n";
    type = "text/plain";
    count = 2;
    if (!valid) { status = 400; body = "bad request\n"; }
    else if (!equal(method, "GET") && !equal(method, "HEAD")) {
        status = 405; body = "method not allowed\n";
        headers[2].name = cu_str_cstr("Allow");
        headers[2].value = cu_str_cstr("GET, HEAD");
        count = 3;
    } else if (equal(target, "/status")) {
        type = "application/json";
        if (state == 1) body = "{\"status\":\"ready\"}\n";
        else {
            status = 503;
            body = state == 0 ? "{\"status\":\"checking\"}\n" :
                                "{\"status\":\"failed\"}\n";
        }
    } else if (!equal(target, "/health")) { status = 404; body = "not found\n"; }
    headers[0].name = cu_str_cstr("Date");
    headers[0].value = cu_str_cstr(date);
    headers[1].name = cu_str_cstr("Content-Type");
    headers[1].value = cu_str_cstr(type);
    length = strlen(body);
    *written = 0;
    if (cu_http_response_head(wire, cap, status, length, headers, count, &head)
        != CU_HTTP_RESPONSE_OK) return 0;
    if (equal(method, "HEAD")) length = 0;
    if (length > cap - head) return 0;
    memcpy(wire + head, body, length);
    *written = head + length;
    return 1;
}

int health_connection(CuSocket *socket, unsigned long timeout, HealthJob *job)
{
    CuReader reader;
    CuHttpRequest request;
    CuHttpResult result;
    char head[2048];
    char wire[1024];
    char date[64];
    char byte;
    size_t n;
    size_t length;
    size_t i;
    time_t now;
    struct tm *utc;
    int valid;
    CuStr method;
    CuStr target;
    method = cu_str_empty(); target = cu_str_empty();
    if (!cu_reader_socket(&reader, socket, timeout)) return 0;
    /* Failed begin clears metadata. Retain only an unmistakable HEAD prefix
     * for error-response body suppression, never for accepting input. */
    for (i = 0; i < 5; ++i) head[i] = '\0';
    result = cu_http_begin(&request, &reader, head, sizeof head, 16, 0);
    valid = result == CU_HTTP_OK;
    if (!valid && memcmp(head, "HEAD ", 5) == 0) method = cu_str_cstr("HEAD");
    if (valid) {
        if (cu_http_read(&request, &byte, 1, &n) != CU_HTTP_END || n != 0) return 0;
        method = request.method; target = request.target;
    }
    /* Only the serving thread uses gmtime/strftime; the worker never does.
     * The executable keeps the startup C locale for English HTTP dates. */
    now = time(NULL);
    utc = gmtime(&now);
    if (utc == NULL || strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", utc) == 0)
        return 0;
    if (!health_wire(wire, sizeof wire, &length, method, target, valid,
                     health_snapshot(job), date)) return 0;
    if (!cu_socket_set_timeout(socket, timeout)) return 0;
    return cu_socket_write(socket, cu_str_n(wire, length));
}

int health_serve(unsigned port, unsigned long requests, unsigned long timeout,
                 unsigned long rounds)
{
    CuArena arena;
    CuThread *thread;
    CuPoller *poller;
    CuSocket listener;
    CuSocket client;
    HealthJob job;
    CuReady ready;
    size_t n;
    unsigned long accepted;
    int ok;
    int watched;
    int sent;
    cu_arena_init(&arena);
    job.rounds = rounds; job.state = 0; job.stop = 0;
    if (!cu_mutex_init(&job.mutex, &arena)) { cu_arena_release(&arena); return 1; }
    if (!cu_thread_start(&thread, &arena, 0, health_worker, &job)) {
        cu_mutex_destroy(job.mutex); cu_arena_release(&arena); return 1;
    }
    poller = NULL;
    ok = cu_socket_listen(&listener, "127.0.0.1", port, 8);
    if (ok) ok = cu_poller_init(&poller, &arena, 1);
    if (ok) {
        printf("127.0.0.1:%u\n", cu_socket_port(&listener));
        if (fflush(stdout) != 0) ok = 0;
    }
    watched = 0; accepted = 0;
    while (ok && !interrupted && accepted < requests) {
        if (!watched) {
            ok = cu_poller_add(poller, &listener, NULL, timeout);
            if (!ok) break;
            watched = 1;
        }
        ok = cu_poller_wait(poller, 25, &ready, 1, &n);
        if (!ok || n == 0) continue;
        watched = 0;
        if (ready.expired || interrupted) break;
        /* Failed accept leaves client untouched: there is nothing to close. */
        if (!cu_socket_accept(&listener, &client)) continue;
        ++accepted;
        sent = health_connection(&client, timeout, &job);
        if (!cu_socket_close(&client)) sent = 0;
        /* A failed peer is isolated; finite accepted count still advances. */
        (void)sent;
    }
    if (poller != NULL) cu_poller_destroy(poller);
    if (!cu_socket_close(&listener)) ok = 0;
    cu_mutex_lock(job.mutex); job.stop = 1; cu_mutex_unlock(job.mutex);
    cu_thread_join(thread);
    cu_mutex_destroy(job.mutex);
    cu_arena_release(&arena);
    return ok ? 0 : 1;
}

/* Signal handlers only set a flag; no library or datetime operations. */
int health_signals(void)
{
    interrupted = 0;
#if defined(__unix__) && !defined(__wasi__) && !defined(__cerb__)
    /* A closed readiness pipe must reach normal worker join/cleanup too. */
    return signal(SIGPIPE, SIG_IGN) != SIG_ERR &&
           signal(SIGINT, health_signal) != SIG_ERR &&
           signal(SIGTERM, health_signal) != SIG_ERR &&
           signal(SIGHUP, health_signal) != SIG_ERR;
#else
    return 0;
#endif
}
