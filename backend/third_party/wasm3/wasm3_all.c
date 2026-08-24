/* wasm3_all.c — amalgamated wasm3 interpreter (single translation unit).
 * wasm3 is vendored third-party code; compiler warnings are suppressed here
 * so the project's -Wall -Wextra builds stay clean. This is exactly the
 * amalgamation wasm3's own scripts/amalgamate.py produces. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"

#include "m3_config.h"
#include "m3_core.c"
#include "m3_bind.c"
#include "m3_code.c"
#include "m3_compile.c"
#include "m3_env.c"
#include "m3_exec.c"
#include "m3_function.c"
#include "m3_info.c"
#include "m3_module.c"
#include "m3_parse.c"
#include "m3_validate.c"
#include "m3_api_libc.c"

#pragma clang diagnostic pop
