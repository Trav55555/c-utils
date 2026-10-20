#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "cu.h"

#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
#include <time.h>

typedef struct Service {
    CuMutex *mutex;
    int active;
    int peak;
    int entered;
    int finished;
} Service;

typedef struct Connection {
    CuSocket socket;
    Service *service;
    const char *expected;
    int finished;
    int ok;
} Connection;

static void pause_tick(void)
{
    struct timespec t;

    t.tv_sec = 0;
    t.tv_nsec = 1000000L;
    while (nanosleep(&t, &t) != 0) assert(errno == EINTR);
}

static int request(CuTask *task, Connection *connection)
{
    char response[128];
    size_t response_length;
    CuReader reader;
    CuHttpRequest http;
    CuHttpResult status;
    char head[128];
    char body[3];
    size_t got;
    size_t used;
    unsigned long left;

    if (!cu_task_remaining(task, &left)) return 0;
    if (!cu_reader_socket(&reader, &connection->socket, left)) return 0;
    status = cu_http_begin(&http, &reader, head, sizeof head, 2, 3);
    if (status != CU_HTTP_OK) {
        errno = status == CU_HTTP_IO ? cu_http_errno(&http) : EPROTO;
        return 0;
    }
    used = 0;
    while (http.remaining != 0) {
        status = cu_http_read(&http, body + used, sizeof body - used, &got);
        if (status != CU_HTTP_OK) {
            errno = status == CU_HTTP_IO ? cu_http_errno(&http) : EPROTO;
            return 0;
        }
        assert(got > 0 && got <= sizeof body - used);
        used += got;
    }
    if (used != 3 || memcmp(body, connection->expected, 3) != 0) {
        errno = EPROTO;
        return 0;
    }
    if (cu_http_response_head(response, sizeof response, 200, sizeof body, NULL, 0,
                              &response_length) != CU_HTTP_RESPONSE_OK) return 0;
    if (!cu_task_remaining(task, &left) ||
        !cu_socket_set_timeout(&connection->socket, left) ||
        !cu_socket_write(&connection->socket, cu_str_n(response, response_length))) return 0;
    if (!cu_task_remaining(task, &left) ||
        !cu_socket_set_timeout(&connection->socket, left) ||
        !cu_socket_write(&connection->socket, cu_str_n(body, used))) return 0;
    return 1;
}

static int serve(CuTask *task, void *context)
{
    Connection *connection;
    Service *service;
    int ready;
    int ok;
    int error;

    connection = (Connection *)context;
    service = connection->service;
    cu_mutex_lock(service->mutex);
    service->active++;
    service->entered++;
    if (service->active > service->peak) service->peak = service->active;
    cu_mutex_unlock(service->mutex);
    do {
        cu_mutex_lock(service->mutex);
        ready = service->entered >= 2;
        cu_mutex_unlock(service->mutex);
        if (ready || cu_task_cancelled(task)) break;
        pause_tick();
    } while (1);
    ok = ready ? request(task, connection) : 0;
    error = errno;
    connection->ok = ok;
    connection->finished = 1;
    cu_mutex_lock(service->mutex);
    service->active--;
    service->finished++;
    cu_mutex_unlock(service->mutex);
    errno = error;
    return ok;
}

static void check_service(int mode)
{
    static const char head[] = "POST /echo HTTP/1.1\r\nHost: example.test\r\nContent-Length: 3\r\n\r\n";
    static const char response[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n\r\n";
    static const char bad[] = "POST /echo HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\nabc";
    CuArena control;
    CuArena sync;
    CuSocket listener;
    CuSocket clients[4];
    Connection connections[4];
    CuTaskSpec jobs[4];
    Service service;
    char reply[sizeof response + 2];
    size_t count;
    size_t i;
    size_t used;
    size_t got;
    int ok;
    int error;
    int closed;

    cu_arena_init(&control);
    cu_arena_init(&sync);
    assert(cu_mutex_init(&service.mutex, &sync));
    service.active = service.peak = service.entered = service.finished = 0;
    assert(cu_socket_listen(&listener, "127.0.0.1", 0, 8));
    count = mode == 0 ? 4 : 2;
    for (i = 0; i < count; i++) {
        assert(cu_socket_connect_ip_for(&clients[i], "127.0.0.1", cu_socket_port(&listener), 1000));
        assert(cu_socket_set_timeout(&clients[i], 2000));
        assert(cu_socket_accept(&listener, &connections[i].socket));
        connections[i].service = &service;
        connections[i].expected = i % 2 == 0 ? "one" : "two";
        connections[i].finished = connections[i].ok = 0;
        jobs[i].fn = serve;
        jobs[i].context = &connections[i];
        if (mode == 1 && i == 0) {
            assert(cu_socket_write(&clients[i], cu_str_cstr(bad)));
        } else if (!(mode == 2 && i == 1)) {
            assert(cu_socket_write(&clients[i], cu_str_cstr(head)));
            assert(cu_socket_write(&clients[i], cu_str_n(connections[i].expected, 3)));
        }
    }
    ok = cu_tasks_run(&control, jobs, count, 2, mode == 2 ? 120 : 2000);
    error = errno;
    assert(service.active == 0 && service.peak <= 2);
    if (mode == 0) {
        assert(ok && service.peak == 2 && service.entered == 4 && service.finished == 4);
        for (i = 0; i < count; i++) {
            assert(connections[i].ok && connections[i].finished);
            used = 0;
            while (used < sizeof reply) {
                assert(cu_socket_read(&clients[i], reply + used, sizeof reply - used, &got));
                assert(got > 0 && got <= sizeof reply - used);
                used += got;
            }
            assert(memcmp(reply, response, sizeof response - 1) == 0);
            assert(memcmp(reply + sizeof response - 1, connections[i].expected, 3) == 0);
        }
    } else {
        assert(!ok && error == (mode == 1 ? EPROTO : ETIMEDOUT));
        assert(service.finished == service.entered);
    }
    /* The scope owner closes every accepted socket, including jobs skipped
     * by cancellation. No worker can still be using them after run returns. */
    for (i = 0; i < count; i++) {
        closed = cu_socket_close(&connections[i].socket);
        assert(closed || cu_socket_errno(&connections[i].socket) == ETIMEDOUT);
        assert(cu_socket_close(&clients[i]));
    }
    assert(cu_socket_close(&listener));
    cu_mutex_destroy(service.mutex);
    cu_arena_release(&sync);
    cu_arena_release(&control);
}
#endif

int main(void)
{
#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
    check_service(0);
    check_service(1);
    check_service(2);
#endif
    return 0;
}
