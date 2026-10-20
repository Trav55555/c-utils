#include "cuhealth.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
int main(int argc, char **argv)
{
    unsigned long port;
    unsigned long requests;
    unsigned long timeout;
    unsigned long rounds;
    unsigned long value;
    unsigned long max;
    int i;
    port = 0; requests = 8; timeout = 1000; rounds = 1000;
    for (i = 1; i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        max = ULONG_MAX;
        if (strcmp(argv[i], "--port") == 0) max = 65535;
        else if (strcmp(argv[i], "--rounds") == 0) max = 1000000;
        else if (strcmp(argv[i], "--requests") != 0 &&
                 strcmp(argv[i], "--timeout-ms") != 0) goto usage;
        if (!health_number(argv[i + 1], max, &value)) goto usage;
        if (strcmp(argv[i], "--port") == 0) port = value;
        else {
            if (value == 0) goto usage;
            if (strcmp(argv[i], "--requests") == 0) requests = value;
            else if (strcmp(argv[i], "--rounds") == 0) rounds = value;
            else timeout = value;
        }
    }
    if (!health_signals()) return 1;
    return health_serve((unsigned)port, requests, timeout, rounds);
usage:
    fputs("usage: cuhealth [--port 0..65535] [--requests N] [--timeout-ms N] [--rounds 1..1000000]\n", stderr);
    return 2;
}
