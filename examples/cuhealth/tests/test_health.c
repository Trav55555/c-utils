#include "cuhealth.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void policy(const char *method, const char *path, int valid, int state,
                   const char *expected, const char *body)
{
    char wire[1024];
    size_t n;
    char *end;
    assert(health_wire(wire, sizeof wire - 1, &n, cu_str_cstr(method), cu_str_cstr(path),
                       valid, state, "Thu, 01 Jan 1970 00:00:00 GMT"));
    wire[n] = '\0';
    assert(strstr(wire, expected) != NULL);
    end = strstr(wire, "\r\n\r\n");
    assert(end != NULL);
    assert(strcmp(end + 4, body) == 0);
}
static void socket_case(HealthJob *job, const char *request, int success)
{
    CuSocket listener;
    CuSocket client;
    CuSocket server;
    char reply[1024];
    size_t n;
    assert(cu_socket_listen(&listener, "127.0.0.1", 0, 1));
    assert(cu_socket_connect_ip_for(&client, "127.0.0.1", cu_socket_port(&listener), 1000));
    assert(cu_socket_accept(&listener, &server));
    assert(cu_socket_write(&client, cu_str_cstr(request)));
    assert(health_connection(&server, 20, job) == success);
    /* Timeout errors remain sticky through close. */
    if (success) assert(cu_socket_close(&server));
    else { int closed; closed = cu_socket_close(&server); (void)closed; }
    assert(cu_socket_set_timeout(&client, 1000));
    assert(cu_socket_read(&client, reply, sizeof reply, &n));
    if (success) assert(n > 0);
    assert(cu_socket_close(&client));
    assert(cu_socket_close(&listener));
}

int main(void)
{
    unsigned long n;
    char wire[1];
    size_t written;
    CuArena arena;
    HealthJob job;
    CuThread *thread;
    CuSocket reservation;
    unsigned port;
    assert(health_number("65535", 65535, &n) && n == 65535);
    assert(health_number("0", 0, &n) && n == 0);
    assert(!health_number("", 100, &n));
    assert(!health_number("-1", 100, &n));
    assert(!health_number("101", 100, &n));
    assert(!health_number("9", 8, &n));
    policy("GET", "/health", 1, 0, "200 OK", "ok\n");
    policy("HEAD", "/health", 1, 0, "Content-Length: 3", "");
    policy("GET", "/status", 1, 0, "503", "{\"status\":\"checking\"}\n");
    policy("GET", "/status", 1, 1, "200 OK", "{\"status\":\"ready\"}\n");
    policy("GET", "/status", 1, -1, "503", "{\"status\":\"failed\"}\n");
    policy("HEAD", "/status", 1, 0, "503", "");
    policy("GET", "/unknown", 1, 0, "404", "not found\n");
    policy("POST", "/health", 1, 0, "Allow: GET, HEAD", "method not allowed\n");
    policy("GET", "/health", 0, 0, "400", "bad request\n");
    policy("HEAD", "/health", 0, 0, "400", "");
    assert(!health_wire(wire, sizeof wire, &written, cu_str_cstr("GET"), cu_str_cstr("/health"),
                        1, 0, "Thu, 01 Jan 1970 00:00:00 GMT"));
    cu_arena_init(&arena);
#if defined(__wasi__) || defined(__cerb__)
    assert(!cu_mutex_init(&job.mutex, &arena));
    assert(health_serve(0, 1, 2, 5) == 1);
    cu_arena_release(&arena);
    puts("cuhealth: policy passed; socket/thread backend unsupported");
    return 0;
#endif
    assert(cu_mutex_init(&job.mutex, &arena));
    job.rounds = 5; job.state = 0; job.stop = 0;
    assert(cu_thread_start(&thread, &arena, 0, health_worker, &job));
    cu_thread_join(thread);
    assert(health_snapshot(&job) == 1);
    job.stop = 1; job.state = 0;
    health_worker(&job);
    assert(health_snapshot(&job) == 0);
    socket_case(&job, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 1);
    socket_case(&job, "GET /health HTTP/1.1\r\n\r\n", 1);
    socket_case(&job, "HEAD /health HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n", 1);
    socket_case(&job, "GET /health HTTP/1.1\r\nHost:", 0);
    cu_mutex_destroy(job.mutex);
    assert(health_signals());
    assert(cu_socket_listen(&reservation, "127.0.0.1", 0, 1));
    port = cu_socket_port(&reservation);
    /* Keep the reservation owned: startup must fail without a port race.
     * Python exercises accepted service requests using its advertised port 0. */
    assert(health_serve(port, 1, 1000, 5) == 1);
    assert(cu_socket_close(&reservation));
    cu_arena_release(&arena);
    assert(health_serve(0, 1, 2, 5) == 0);
    cu_thread_fail_at(0);
    assert(health_serve(0, 1, 2, 5) == 1);
    cu_thread_fail_at(1);
    assert(health_serve(0, 1, 2, 5) == 1);
    cu_thread_fail_at((size_t)-1);
    puts("cuhealth unit: pass");
    return 0;
}
