// arch_x86_64.c (v2): the x86_64 stencils, in their own translation unit (their tables are static).
#include "v2/compiler/backend/native/stencil.h"
#include "v2/compiler/backend/native/stencils_x86_64.h"

const NativeArch native_x86_64 = {"x86_64", CPU_X86_64, stencils, runtime_stencils, stencil_externs,
                              STENCIL_SET_HASH};
