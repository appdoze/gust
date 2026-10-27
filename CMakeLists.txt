# Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
cmake_minimum_required(VERSION 3.27)
project(goose C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One C runtime for the compiler, TinyCC, SDL and the gfx library, and the
# one `cl game.c` links by default, so a program can link the same archives.
# The non-debug variant even in Debug builds: a /MTd archive does not link
# cleanly into a plain /MT program.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(goose
    src/main.cpp
    src/process.cpp
    src/version.cpp
    src/process.h
    src/progress.cpp
    src/progress.h
    src/buildfile.h
    src/toolchain.h
    src/build.h
    src/packages.h
    src/json.h
    src/native.h
    src/lsp_rpc.h
    src/lsp_analysis.h
    src/lsp_workspace.h
    src/lsp_features.h
    src/lsp.h
    src/format.h
    src/builtin.h
    src/includes.h
    src/utils.h
    src/small_vector.h
    src/flat_map.h
    src/lexer.h
    src/ast.h
    src/dump.h
    src/clone.h
    src/parser.h
    src/resolve.h
    src/expansion_bind.h
    src/typecheck_expansion.h
    src/builtins.h
    src/typecheck.h
    src/type_queries.h
    src/typecheck_types.h
    src/typecheck_exprs.h
    src/typecheck_flow.h
    src/typecheck_calls.h
    src/typecheck_builtins.h
    src/typecheck_nodes.h
    src/optimize.h
    src/optimize_basecase.h
    src/optimize_tre.h
    src/optimize_loops.h
    src/optimize_reassoc.h
    src/bce.h
    src/codegen.h
    src/codegen_types.h
    src/codegen_frames.h
    src/codegen_values.h
    src/codegen_construct.h
    src/codegen_stmts.h
    src/codegen_calls.h
    src/codegen_builtins.h
    src/codegen_render.h
    src/codegen_emit.h
    src/codegen_stacks.h
    src/codegen_nodes.h
    src/runtime_inline.h
    src/jit.h
    src/runtime/runtime.h
    src/runtime/runtime_impl.h
    src/runtime/runtime_threads.h
    src/runtime/runtime_ext.h
    src/runtime/runtime_os.h
    src/runtime/runtime_net.h
    src/runtime/goose.h
    src/gfx.h
    src/gust_ffi.h
)
# Gust: the compiler's command is gust; the target keeps Goose's name, which
# the rest of this file and upstream's changes to it use.
set_target_properties(goose PROPERTIES OUTPUT_NAME gust)

# Keep argv, CRT file access, filesystem paths and Win32's narrow APIs in
# the same encoding as Goose strings. Windows 10 1903+ applies this before
# the C runtime constructs argv; setlocale() in main would be too late.
if(MSVC)
    target_sources(goose PRIVATE src/goose.manifest)
elseif(WIN32)
    enable_language(RC)
    target_sources(goose PRIVATE src/goose.rc)
    set_property(SOURCE src/goose.rc APPEND PROPERTY OBJECT_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/src/goose.manifest")
endif()

# The compiler's version (src/version.cpp): GOOSE_VERSION where given, as a
# release gives its tag, else `git describe` of this tree at each build.
set(GOOSE_VERSION "" CACHE STRING "The compiler's version; empty for git describe")
set(goose_version_h "${CMAKE_CURRENT_BINARY_DIR}/version/goose_version.h")
add_custom_target(goose_version
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${goose_version_h}"
            "-DVERSION=${GOOSE_VERSION}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake"
    BYPRODUCTS "${goose_version_h}")
add_dependencies(goose goose_version)
target_include_directories(goose PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version")

# The compile runs on a thread of its own, with a larger stack (main.cpp).
find_package(Threads REQUIRED)
target_link_libraries(goose PRIVATE Threads::Threads)
# The library the process runner (process.cpp) is built on.
target_include_directories(goose PRIVATE third_party/subprocess)

# A program run in-process runs on the main thread (main.cpp), whose stack the
# executable sets: 64 MB reserved, as for the compile thread, where Windows
# would give it 1 MB and macOS 8 MB. Only what a program's depth reaches is
# committed. On Windows this is also the reserve of every thread started
# without a size of its own (SDL's, a GPU driver's). A Linux main thread's
# stack comes from the RLIMIT_STACK the process starts with (`ulimit -s`,
# commonly 8 MB), which no link option sets.
set(goose_main_stack 67108864)
if(MSVC)
    target_link_options(goose PRIVATE "/STACK:${goose_main_stack}")
elseif(MINGW)
    target_link_options(goose PRIVATE "LINKER:--stack,${goose_main_stack}")
elseif(APPLE)
    math(EXPR goose_main_stack_hex "${goose_main_stack}" OUTPUT_FORMAT HEXADECIMAL)
    target_link_options(goose PRIVATE "LINKER:-stack_size,${goose_main_stack_hex}")
endif()

# The shader compiler behind embed_shader: third-party C (third_party/
# cute_spirv), always built, so a gfx program typechecks and emits the same C
# with or without SDL.
add_library(goose_shaderc STATIC src/shaderc.c src/shaderc.h src/gfx_blob.h)
target_include_directories(goose_shaderc PRIVATE third_party/cute_spirv)
if(MSVC)
    # ckit.h uses typeof and C11 atomics, which MSVC has only in its latest C
    # mode. /W0 as for TinyCC: third-party warnings are not ours to act on.
    target_compile_options(goose_shaderc PRIVATE /W0 /std:clatest /experimental:c11atomics)
else()
    set_target_properties(goose_shaderc PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(goose_shaderc PRIVATE -w)
endif()
target_link_libraries(goose PRIVATE goose_shaderc)

# The in-process C backend: TinyCC, vendored under third_party/tinycc and
# built into every compiler (cmake/libtcc.cmake).
include(cmake/libtcc.cmake)
if(GUST_TCC_SHARED)
    # Gust: TinyCC beside the compiler, where the loader finds it.
    target_link_libraries(goose PRIVATE libtcc_shared)
    set_target_properties(libtcc_shared PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:goose>"
        LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:goose>")
    if(APPLE)
        set_target_properties(goose PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
        set_target_properties(libtcc_shared PROPERTIES INSTALL_NAME_DIR "@rpath")
    elseif(UNIX)
        set_target_properties(goose PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()
else()
    target_link_libraries(goose PRIVATE libtcc)
endif()
add_dependencies(goose tcc_runtime)
# A packager sets this to where it installs tcclib/, so the binary names no
# build directory. A tcclib/ beside the compiler still wins (src/jit.h).
set(GOOSE_TCC_LIB_PATH "${TCC_STAGE}" CACHE PATH "TinyCC's runtime directory")
target_compile_definitions(goose PRIVATE GOOSE_TCC_LIB_PATH="${GOOSE_TCC_LIB_PATH}")

# Gust: the other in-process backend, `gust run --mir`: MIR's C front end
# and optimizing generator, vendored under third_party/mir (MIT, src/jit_mir.h).
add_library(goose_mir STATIC
    third_party/mir/mir.c
    third_party/mir/mir-gen.c
    third_party/mir/c2mir/c2mir.c)
target_include_directories(goose_mir PUBLIC third_party/mir)
set_target_properties(goose_mir PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(WIN32)
    # MIR's built-in headers test __WIN32, which MinGW's gcc defines and
    # MSVC and clang do not: without it c2mir's stdarg.h declares no va_list.
    target_compile_definitions(goose_mir PRIVATE __WIN32=1)
endif()
if(MSVC)
    target_compile_options(goose_mir PRIVATE /w)
    target_compile_definitions(goose_mir PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(goose_mir PRIVATE -w)
endif()
target_link_libraries(goose PRIVATE goose_mir)
if(UNIX)
    target_link_libraries(goose PRIVATE ${CMAKE_DL_LIBS})
endif()
# The runtime compiled natively into the compiler (src/runtime_native.c),
# which a --mir program links against by name: exported from the executable,
# on Windows by its dllexport and on Linux by the dynamic list (macOS's
# linker exports an executable's symbols as it is).
add_library(goose_rt_native OBJECT src/runtime_native.c)
set_target_properties(goose_rt_native PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(MSVC)
    target_compile_options(goose_rt_native PRIVATE /w /utf-8)
    target_compile_definitions(goose_rt_native PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(goose_rt_native PRIVATE -w)
endif()
target_link_libraries(goose PRIVATE goose_rt_native)
if(UNIX AND NOT APPLE)
    target_link_options(goose PRIVATE
        "LINKER:--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/cmake/runtime_exports.list")
endif()

# The blessed packages GOOSE_BUILTIN_LIBS names (src/blessed.txt; `all`, or
# a list of their names), compiled into the compiler (cmake/package.cmake),
# so that their programs run with nothing else installed. Empty by default:
# the compiler builds alone, and a program importing one of them has it
# fetched and built into GOOSE_HOME on first use.
set(GOOSE_BUILTIN_LIBS "" CACHE STRING
    "Blessed packages to compile into the compiler: all, or a list of their names")
include(cmake/package.cmake)
goose_builtin_packages("${GOOSE_BUILTIN_LIBS}")
if(MSVC AND TARGET goose_gfx)
    # The system DLLs static SDL brings into goose with the gfx package:
    # delay-loaded, so that a compiler run which never opens a window does
    # not load them.
    foreach(dll IN ITEMS user32 gdi32 winmm imm32 ole32 oleaut32 version setupapi shell32)
        target_link_options(goose PRIVATE "/DELAYLOAD:${dll}.dll")
    endforeach()
    target_link_libraries(goose PRIVATE delayimp)
endif()

if(MSVC)
    # /bigobj: the compiler is one translation unit of header-inline
    # functions, past the object format's default COMDAT section limit.
    target_compile_options(goose PRIVATE /W4 /permissive- /utf-8 /EHsc /bigobj)
    target_compile_definitions(goose PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # -Wmissing-field-initializers (in -Wextra) fires on every aggregate
    # initialised with a prefix of its fields, which is deliberate throughout:
    # the rest are value-initialised.
    target_compile_options(goose PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
