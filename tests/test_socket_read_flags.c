#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void) { return 0; }
#else
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "cu.h"

/* Use the real public APIs and a dup alias of the same open description.
 * The one-byte peek proves read_for did not read ahead into the second byte. */
static void flags_case(int nonblock)
{
    CuSocket listener;
    CuSocket client;
    CuSocket server;
    int alias;
    int before;
    int descriptor_flags;
    char byte;
    size_t n;
    unsigned attempts;
    struct pollfd ready;

    assert(cu_socket_listen(&listener, "127.0.0.1", 0, 4));
    assert(cu_socket_connect_ip_for(&client, "127.0.0.1", cu_socket_port(&listener), 1000));
    /* Connect completion need not publish accept readiness immediately. */
    for (attempts = 0; attempts < 100; attempts++) {
        if (cu_socket_accept(&listener, &server))
            break;
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        ready.fd = listener.fd;
        ready.events = POLLIN;
        ready.revents = 0;
        assert(poll(&ready, 1, 10) >= 0 || errno == EINTR);
    }
    assert(attempts < 100);
    alias = dup(server.fd);
    assert(alias >= 0);
    before = fcntl(alias, F_GETFL);
    assert(before >= 0);
    if (nonblock) {
        assert(fcntl(alias, F_SETFL, before | O_NONBLOCK) == 0);
        before |= O_NONBLOCK;
    }
    descriptor_flags = fcntl(server.fd, F_GETFD);
    assert(descriptor_flags >= 0);
    assert(cu_socket_write(&client, CU_LIT("ab")));
    assert(cu_socket_read_for(&server, &byte, 1, &n, 1000) && n == 1 && byte == 'a');
    assert(recv(alias, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 1 && byte == 'b');
    assert(cu_socket_read_for(&server, &byte, 1, &n, 1000) && n == 1 && byte == 'b');
    assert(fcntl(alias, F_GETFL) == before && fcntl(server.fd, F_GETFL) == before);
    assert(fcntl(server.fd, F_GETFD) == descriptor_flags);
    assert(!cu_socket_read_for(&server, &byte, 1, &n, 10));
    assert(cu_socket_errno(&server) == ETIMEDOUT);
    assert(fcntl(alias, F_GETFL) == before && fcntl(server.fd, F_GETFD) == descriptor_flags);
    assert(!cu_socket_close(&server));
    assert(close(alias) == 0);
    assert(cu_socket_close(&client) && cu_socket_close(&listener));
}

int main(void)
{
    flags_case(0);
    flags_case(1);
    return 0;
}
#endif
