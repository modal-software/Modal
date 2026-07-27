#ifndef SIGNAL_H
#define SIGNAL_H
#include <stddef.h>

typedef enum
{
    // start of a sending
    SIG_START,
    // operation in progress
    SIG_DELIVERYING,
    SIG_DELIVERED, /* stage finished successfully                 */
    SIG_ERROR,     /* stage finished with a failure                */
} SignalState;

typedef enum
{
    SIG_OK,
    SIG_WARN,
    SIG_FAIL,
} SignalStatus;

typedef struct Signal
{
    int kind; /* namespaced tag, see signal.kinds.h */
    SignalState state;
    SignalStatus status;
    void *payload; /* polymorphic - no fixed union */
    size_t size;
} Signal;

typedef void (*Sub)(Signal sig, void *cx);

typedef struct Route
{
    Sub handler;
    void *cx;
} Route;

#define SIGNAL_MAX_KIND 512
#define SIGNAL_MAX_SUBS 16
#define SIGNAL_KIND_ANY (SIGNAL_MAX_KIND - 1)

/* KIND_ANY (-1) is stored in slot 0; every real kind is offset by +1
   so array indexing stays a flat, branch-free lookup. */
#define SIGNAL_SLOT(kind) ((size_t)((kind) + 1))

typedef struct Relay
{
    Route routes[SIGNAL_MAX_KIND + 1][SIGNAL_MAX_SUBS];
    size_t count[SIGNAL_MAX_KIND + 1];
} Relay;

/* A Component is a self-installing unit - the builder-pattern piece.
   Each module (tokenizer, parser, a backend, test_runner, tracer)
   exposes one; attaching it wires its own subs into the Relay without
   the Relay ever knowing who they are. */
typedef struct Component
{
    const char *name;
    void (*install)(Relay *relay, void *cx);
    void *cx;
} Component;

#endif
