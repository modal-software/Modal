// arch_arm64.c (v2): the arm64 stencils, in their own translation unit (their tables are static).
#include "v2/compiler/backend/native/stencil.h"
#include "v2/compiler/backend/native/stencils_arm64.h"

const NativeArch native_arm64 = {
    "arm64", CPU_ARM64, stencils, runtime_stencils, stencil_externs, STENCIL_SET_HASH,
};
