#include <stddef.h>
#include <stdio.h>
#include "curun.h"

#if defined(__linux__) && !defined(__cerb__) && !defined(__wasi__)
extern char **environ;
#endif

int main(int argc, char *argv[])
{
    /* Forwarding must not leave buffered bytes to flush after SIGPIPE policy
     * is restored. These streams belong to this standalone process. */
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0 ||
        setvbuf(stderr, NULL, _IONBF, 0) != 0)
        return 125;
#if defined(__linux__) && !defined(__cerb__) && !defined(__wasi__)
    return curun_main(argc, argv, environ);
#else
    return curun_main(argc, argv, NULL);
#endif
}
