#ifndef CUHEALTH_H
#define CUHEALTH_H
#include "cu.h"
typedef struct HealthJob {
    CuMutex *mutex;
    unsigned long rounds;
    int state;
    int stop;
} HealthJob;
int health_signals(void);
int health_number(const char *text, unsigned long max, unsigned long *out);
void health_worker(void *context);
int health_snapshot(HealthJob *job);
int health_wire(char *wire, size_t cap, size_t *written, CuStr method,
                CuStr target, int valid, int state, const char *date);
int health_connection(CuSocket *socket, unsigned long timeout, HealthJob *job);
int health_serve(unsigned port, unsigned long requests, unsigned long timeout,
                 unsigned long rounds);
#endif
