#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void) { return 0; }
#else
#include "death.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define cu_socket_errno boundary_errno
#define cu_socket_listen boundary_listen
#define cu_socket_connect boundary_connect
#define cu_socket_connect_ip_for boundary_ip
#define cu_socket_accept boundary_accept
#define cu_socket_port boundary_port
#define cu_socket_set_timeout boundary_timeout
#define cu_socket_read boundary_read
#define cu_socket_read_for boundary_read_for
#define cu_socket_write boundary_write
#define cu_socket_shutdown_write boundary_shutdown_write
#define cu_socket_close boundary_close
#define cu_poller_init boundary_poller_init
#define cu_poller_add boundary_poller_add
#define cu_poller_wait boundary_poller_wait
#define cu_poller_wake boundary_poller_wake
#define cu_poller_destroy boundary_poller_destroy
#include "cu.h"

/* Model identities, never OS descriptors. Every boundary checks death mode
 * before asserting: a fixture assertion must not counterfeit a guard abort. */
enum { CREATE_BAD = 1, CONFIG_BAD, GET_BAD, SET_BAD, OPT_BAD, POLL_BAD,
       CLOCK_BAD, RESTORE_BAD, CONNECT_BAD };
static int guard_active, fault, fault_error, created, connects, polls, clocks;
static int live_fd[8], closed_fd[8], model_flags[8], configured[8];
static int initial_error, interrupt_connect, interrupt_poll, pending_option;
static int option_error, bad_length, untouched_option, spurious, close_error, poll_zero;
static int expected_family, expected_delay, last_delay, restore_count, clock_bad_at;
static short ready_mask;
static const char *literal;
static unsigned expected_port;
static unsigned long virtual_ms, poll_tick, connect_tick, restore_tick;
static void boundary_guard(void) { if (guard_active) _exit(95); }
static int index_fd(int fd)
{
    boundary_guard();
    assert(fd >= 40 && fd < 48 && live_fd[fd - 40]);
    return fd - 40;
}
static int model_socket(int family, int type, int protocol)
{
    int index;
    boundary_guard();
    assert(family == expected_family && protocol == 0);
#ifdef __linux__
    assert(type == (SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK));
#else
    assert(type == SOCK_STREAM);
#endif
    if (fault == CREATE_BAD) { errno = fault_error; return -1; }
    index = created++;
    assert(index < 8);
    live_fd[index] = 1;
    model_flags[index] = O_APPEND;
#ifdef __linux__
    model_flags[index] |= O_NONBLOCK;
#endif
    return 40 + index;
}
static int model_close(int fd)
{
    int index;
    index = index_fd(fd);
    live_fd[index] = 0; closed_fd[index]++;
    errno = close_error;
    return close_error == ERANGE ? 0 : -1;
}
static int model_fcntl(int fd, int command, ...)
{
    int index, value;
    va_list args;
    index = index_fd(fd);
    if (command == F_GETFL) {
        if (fault == GET_BAD) { errno = fault_error; return -1; }
        return model_flags[index];
    }
    va_start(args, command); value = va_arg(args, int); va_end(args);
    if (command == F_SETFD) {
        assert(value == FD_CLOEXEC);
        if (fault == CONFIG_BAD) { errno = fault_error; return -1; }
        configured[index] = 1;
    } else {
        assert(command == F_SETFL && (value & O_APPEND));
        if (!(value & O_NONBLOCK)) {
            restore_count++; virtual_ms += restore_tick;
            if (fault == RESTORE_BAD) { errno = fault_error; return -1; }
        }
        if (fault == SET_BAD) { errno = fault_error; return -1; }
        model_flags[index] = value;
    }
    return 0;
}
static int model_connect(int fd, const struct sockaddr *addr, socklen_t length)
{
    int index;
    struct in_addr v4;
    struct in6_addr v6;
    const struct sockaddr_in *a4;
    const struct sockaddr_in6 *a6;
    index = index_fd(fd);
    assert(configured[index] && (model_flags[index] & O_NONBLOCK));
    /* One connect per created descriptor, including the EINTR retry. */
    assert(connects == index);
    connects++; virtual_ms += connect_tick;
    if (expected_family == AF_INET) {
        assert(length == sizeof *a4);
        a4 = (const struct sockaddr_in *)addr;
        assert(a4->sin_family == expected_family);
        assert(a4->sin_port == htons((unsigned short)expected_port));
        assert(inet_pton(AF_INET, literal, &v4) == 1);
        assert(memcmp(&v4, &a4->sin_addr, sizeof v4) == 0);
    } else {
        assert(length == sizeof *a6);
        a6 = (const struct sockaddr_in6 *)addr;
        assert(a6->sin6_family == expected_family);
        assert(a6->sin6_port == htons((unsigned short)expected_port));
        assert(a6->sin6_scope_id == 0);
        assert(inet_pton(AF_INET6, literal, &v6) == 1);
        assert(memcmp(&v6, &a6->sin6_addr, sizeof v6) == 0);
    }
    if (fault == CONNECT_BAD) { errno = fault_error; return -1; }
    if (interrupt_connect && connects == 1) { errno = EINTR; return -1; }
    if (initial_error) { errno = initial_error; return -1; }
    return 0;
}
static int model_poll(struct pollfd *fds, nfds_t count, int delay)
{
    boundary_guard();
    assert(count == 1 && ++polls < 20);
    (void)index_fd(fds[0].fd);
    assert(fds[0].events == POLLOUT && fds[0].revents == 0);
    assert(delay == -1 || delay > 0);
    if (expected_delay) assert(delay == expected_delay);
    if (last_delay > 0 && delay > 0) assert(delay <= last_delay);
    last_delay = delay;
    virtual_ms += poll_tick;
    if (fault == POLL_BAD) { errno = fault_error; return -1; }
    if (interrupt_poll && polls == 1) { errno = EINTR; return -1; }
    if (poll_zero < 0 || (poll_zero && polls == 1)) return 0;
    fds[0].revents = spurious && polls == 1 ? 0 : ready_mask;
    return 1;
}
static int model_getsockopt(int fd, int level, int name, void *value, socklen_t *length)
{
    (void)index_fd(fd);
    assert(level == SOL_SOCKET && name == SO_ERROR && *length == sizeof(int));
    assert(*(int *)value != 0);
    if (fault == OPT_BAD) { errno = fault_error; return -1; }
    if (!untouched_option) *(int *)value = pending_option && polls == 1 ? pending_option : option_error;
    if (bad_length) *length = 0;
    errno = EDOM;
    return 0;
}
static int model_clock(clockid_t id, struct timespec *stamp)
{
    boundary_guard(); assert(id == CLOCK_MONOTONIC); clocks++;
    if (fault == CLOCK_BAD && (!clock_bad_at || clocks == clock_bad_at)) {
        errno = fault_error; return -1;
    }
    stamp->tv_sec = (time_t)(virtual_ms / 1000UL);
    stamp->tv_nsec = (long)((virtual_ms % 1000UL) * 1000000UL);
    return 0;
}
static int model_dns(const char *host, const char *service,
                     const struct addrinfo *hints, struct addrinfo **result)
{
    (void)host; (void)service; (void)hints; (void)result;
    _exit(95);
    return EAI_FAIL;
}
#ifdef SO_NOSIGPIPE
static int model_setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    (void)index_fd(fd);
    assert(level == SOL_SOCKET && name == SO_NOSIGPIPE);
    assert(length == sizeof(int) && *(const int *)value == 1);
    if (fault == CONFIG_BAD) { errno = fault_error; return -1; }
    return 0;
}
#define setsockopt model_setsockopt
#endif
#define socket model_socket
#define connect model_connect
#define close model_close
#define fcntl model_fcntl
#define poll model_poll
#define getsockopt model_getsockopt
#define clock_gettime model_clock
#define getaddrinfo model_dns
#include "../src/cu_socket.c"
#undef socket
#undef connect
#undef close
#undef fcntl
#undef poll
#undef getsockopt
#undef clock_gettime
#undef getaddrinfo
#ifdef SO_NOSIGPIPE
#undef setsockopt
#endif

static void reset(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        live_fd[i] = closed_fd[i] = model_flags[i] = configured[i] = 0;
    }
    guard_active = fault = created = connects = polls = clocks = 0;
    interrupt_connect = interrupt_poll = pending_option = option_error = 0;
    bad_length = untouched_option = spurious = restore_count = poll_zero = 0;
    expected_delay = last_delay = clock_bad_at = 0;
    fault_error = EIO; close_error = ERANGE;
    initial_error = 0; ready_mask = POLLOUT;
    literal = "192.0.2.17"; expected_family = AF_INET; expected_port = 43210;
    virtual_ms = poll_tick = connect_tick = restore_tick = 0;
}
static void run(unsigned long budget, int success, int error)
{
    CuSocket s;
    int result, saved, i;
    result = boundary_ip(&s, literal, expected_port, budget); saved = errno;
    assert(result == success);
    assert(s.listen == 0 && s.watched == 0 && s.timeout_ms == 0);
    if (success) {
        assert(s.state == 1 && s.fd == 40 + created - 1 && s.error == 0);
        assert(!(model_flags[s.fd - 40] & O_NONBLOCK));
        assert(model_flags[s.fd - 40] & O_APPEND);
        assert(restore_count > 0);
    } else {
        assert(saved == error && boundary_errno(&s) == error);
        assert(s.fd == -1 && s.state == -1);
        for (i = 0; i < created; i++) assert(closed_fd[i] == 1);
    }
    result = boundary_close(&s); assert(result == success);
    if (!success) assert(errno == error);
    errno = EDOM;
    result = boundary_close(&s); assert(result == success);
    if (!success) assert(errno == error);
    for (i = 0; i < created; i++) assert(!live_fd[i] && closed_fd[i] == 1);
}
/* Invalid getter output must never be interpreted as successful completion.
 * The contract deliberately does not prescribe its synthetic error number. */
static void malformed_option(int untouched)
{
    CuSocket s;
    int result, saved;
    reset(); initial_error = EINPROGRESS;
    bad_length = !untouched; untouched_option = untouched;
    result = boundary_ip(&s, literal, expected_port, 100);
    saved = errno;
    assert(!result && s.fd == -1 && s.state == -1 && s.error == saved);
    assert(created == 1 && closed_fd[0] == 1);
    result = boundary_close(&s); assert(!result && errno == saved);
    result = boundary_close(&s); assert(!result && closed_fd[0] == 1);
}
#ifdef CU_DEATH_TESTS
static int misuse;
static void death_case(void)
{
    CuSocket s;
    int result;
    reset(); guard_active = 1;
    result = boundary_ip(misuse ? &s : NULL, misuse ? NULL : "127.0.0.1", 1, 10);
    (void)result;
}
#endif
int main(void)
{
    int kind, zero, code, mask;
    size_t i;
    struct timespec start, end;
    char overlong[100];
    static const char *invalid[] = { "", "localhost", "[::1]", "fe80::1%lo", "1.2.3.999", "::g" };
    /* Referenced even while production is the tests-first ENOSYS stub. */
    (void)model_getsockopt;
    reset(); run(0, 1, 0); assert(clocks == 0 && polls == 0);
    reset(); literal = "2001:db8::1234"; expected_family = AF_INET6; run(100, 1, 0);
    reset(); literal = "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255";
    assert(strlen(literal) == 45); expected_family = AF_INET6; run(100, 1, 0);
    reset(); expected_port = 65535; run(100, 1, 0);
    reset(); expected_port = 0; run(100, 1, 0);
    for (code = 0; code < 2; code++) {
        reset(); initial_error = code ? EALREADY : EINPROGRESS;
        run(100, 1, 0); assert(connects == 1 && polls == 1);
        reset(); initial_error = EINPROGRESS; pending_option = code ? EALREADY : EINPROGRESS;
        run(100, 1, 0); assert(connects == 1 && polls == 2);
    }
    for (mask = 0; mask < 3; mask++) {
        reset(); initial_error = EINPROGRESS;
        ready_mask = mask == 0 ? POLLOUT : (mask == 1 ? POLLHUP : POLLERR);
        option_error = ECONNREFUSED; run(100, 0, ECONNREFUSED);
        if (mask) { reset(); initial_error = EINPROGRESS;
            ready_mask = mask == 1 ? POLLHUP : POLLERR; run(100, 0, ECONNABORTED); }
    }
    reset(); initial_error = EINPROGRESS; ready_mask = POLLNVAL; run(100, 0, EBADF);
    reset(); initial_error = EINPROGRESS; spurious = 1; run(100, 1, 0); assert(polls == 2);
    reset(); initial_error = EINPROGRESS; poll_zero = 1; poll_tick = 3;
    run(100, 1, 0); assert(polls == 2 && virtual_ms == 6);
    reset(); initial_error = EINPROGRESS; poll_zero = -1; poll_tick = 3;
    run(5, 0, ETIMEDOUT); assert(polls == 2 && virtual_ms == 6);
    reset(); initial_error = EINPROGRESS; interrupt_poll = 1; poll_tick = 3;
    run(5, 0, ETIMEDOUT); assert(polls == 2 && connects == 1 && virtual_ms == 6);
    reset(); interrupt_connect = 1; connect_tick = 3; run(5, 0, ETIMEDOUT);
    assert(created == 2 && connects == 2 && virtual_ms == 6);
    reset(); interrupt_connect = 1; initial_error = EINPROGRESS;
    connect_tick = 2; poll_tick = 2; run(5, 0, ETIMEDOUT);
    assert(created == 2 && connects == 2 && polls == 1);
    reset(); connect_tick = 6; run(5, 0, ETIMEDOUT);
    reset(); restore_tick = 6; run(5, 0, ETIMEDOUT);
    reset(); initial_error = ECONNREFUSED; connect_tick = 6; run(5, 0, ECONNREFUSED);
    reset(); initial_error = EAGAIN; run(100, 0, EAGAIN); assert(connects == 1);
    for (kind = CREATE_BAD; kind <= CONNECT_BAD; kind++) {
        for (zero = 0; zero < 2; zero++) {
            reset(); fault = kind; fault_error = zero ? 0 : EIO;
            close_error = EINTR; initial_error = EINPROGRESS;
            run(100, 0, fault_error);
        }
    }
    malformed_option(0); malformed_option(1);
    for (zero = 0; zero < 2; zero++) {
        reset(); fault = CLOCK_BAD; clock_bad_at = 2;
        fault_error = zero ? 0 : EIO; run(100, 0, fault_error);
    }
    for (code = 3; code <= 6; code++) {
        for (zero = 0; zero < 2; zero++) {
            reset(); initial_error = EINPROGRESS;
            fault = CLOCK_BAD; clock_bad_at = code; fault_error = zero ? 0 : EIO;
            run(100, 0, fault_error);
            assert(created == 1 && closed_fd[0] == 1);
            if (code == 6) assert(restore_count == 1);
        }
    }
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        reset(); literal = invalid[i]; run(100, 0, EINVAL); assert(created == 0);
    }
    memset(overlong, '1', sizeof overlong); overlong[sizeof overlong - 1] = '\0';
    reset(); literal = overlong; run(100, 0, EINVAL); assert(created == 0);
    reset(); expected_port = 65536; run(100, 0, EINVAL); assert(created == 0);
    reset(); initial_error = EINPROGRESS; expected_delay = INT_MAX;
    run(ULONG_MAX, 1, 0);
    reset(); initial_error = EINPROGRESS; expected_delay = INT_MAX;
    run((unsigned long)INT_MAX, 1, 0);
    reset(); initial_error = EINPROGRESS; expected_delay = INT_MAX;
    run((unsigned long)INT_MAX + 1UL, 1, 0);
    /* Directly exercise the existing unsigned elapsed-time arithmetic,
     * including a representable signed time_t transition through zero. */
    start.tv_sec = (time_t)-1; start.tv_nsec = 250000000L;
    end.tv_sec = 1; end.tv_nsec = 250000000L;
    assert(remaining_ms(start, end, 3000) == 1000);
    start.tv_sec = 0; start.tv_nsec = 750000000L;
    end.tv_sec = 1; end.tv_nsec = 250000000L;
    assert(remaining_ms(start, end, 1000) == 500);
#ifdef CU_DEATH_TESTS
    for (misuse = 0; misuse < 2; misuse++) expect_abort(death_case);
#endif
    return 0;
}
#endif
