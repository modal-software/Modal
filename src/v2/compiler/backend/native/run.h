// run.h (v2): running the native backend's program in the compiler's own process (see run.c).
#ifndef MODAL_V2_RUN_H
#define MODAL_V2_RUN_H

#include "v2/compiler/backend/native/macho.h"
#include <stddef.h>
#include <stdint.h>

// 1 if code for `cpu` runs in this process (the compiler was built for that CPU).
int run_host_can(MachoCpu cpu);

// Loads `code` (position-independent, `entry` the offset of `int main(void)` in it) into
// executable memory, sends its calls to C library functions (`calls`, by index in the
// NULL-terminated `imports`) to the ones of this process, and calls the entry.
// Returns the program's exit status as a child process would give it: main's result, or 1 if it
// was stopped by a fault. -1 if it could not be loaded (the caller writes a file instead).
int run_in_memory(MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                  const MachoImportCall *calls, size_t ncalls, const char *const *imports);

#endif // MODAL_V2_RUN_H
