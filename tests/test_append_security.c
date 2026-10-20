#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "cu.h"

#if (defined(__unix__) || defined(__APPLE__)) && !defined(__cerb__) && !defined(__wasi__)
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* An executed child must not receive the parent's append capability. */
int main(int argc, char **argv)
{
    CuAppend f;
    pid_t child;
    int status;
    char fd[32];

    if (argc == 2) {
        errno = 0;
        return fcntl(atoi(argv[1]), F_GETFD) == -1 && errno == EBADF ? 0 : 1;
    }
    assert(cu_append_open(&f, "/dev/null", 0));
    sprintf(fd, "%d", f.fd);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], fd, (char *)NULL);
        _exit(2);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(cu_append_close(&f));
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}
#else
int main(void)
{
    return 0;
}
#endif
