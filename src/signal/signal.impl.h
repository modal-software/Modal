#ifndef SIGNAL_IMPL_H
#define SIGNAL_IMPL_H
#include "signal.h"

typedef struct
{
    Signal (*new)(int kind, void *payload, size_t size);
    void (*on)(Relay *relay, int kind, Sub handler, void *ctx);
    void (*emit)(Relay *relay, Signal sig);
    void (*attach)(Relay *relay, Component c);
} SignalImpl;

extern const SignalImpl signal;

#endif
