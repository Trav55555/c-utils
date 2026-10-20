#ifndef CURUN_H
#define CURUN_H

/* Standalone CLI entry. argv is a borrowed NUL-terminated vector with argv[0];
 * envp is borrowed and NUL-terminated, or NULL for an empty environment.
 * Runs synchronously, serially, with exclusive ownership of its direct child.
 * See README.md for limits, cancellation latency and exit-code policy. */
int curun_main(int argc, char *argv[], char *envp[]);

#endif
