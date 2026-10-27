/* Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
   The runtime object (goose --emit-runtime) compiled into the compiler, for
   gust run --mir: MIR compiles only the program's own unit, which declares
   the runtime extern, and links it against these functions (src/jit_mir.h).
   Native code for the runtime, then, and no system headers but the C
   library's for c2mir to read, which is what lets --mir run on Windows.

   GS_NATIVE_RUNTIME exports what the runtime defines (runtime.h's GS_API),
   for MIR to find by name in the compiler's own executable; ends the
   program's process as an in-process run does (gs_end); and takes the
   resource directory at run time (runtime_os.h). The runtime's text the
   compiler embeds (runtime_inline.h) and the files this includes have to
   be the same: test/run_tests.py checks it. Without GS_RUNTIME_VERSION its
   start is plain gs_rt_start, which MIR finds for any version's name. */

#define GS_SEPARATE_RUNTIME 1
#define GS_RUNTIME_OBJECT 1
#define GS_NEED_THREADS 1
#define GS_NATIVE_RUNTIME 1

#include "runtime/runtime.h"
#include "runtime/runtime_impl.h"
#include "runtime/runtime_threads.h"
#include "runtime/runtime_ext.h"
#include "runtime/runtime_os.h"
#include "runtime/runtime_net.h"
