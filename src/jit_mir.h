// Gust: the MIR backend for in-process runs (`gust run --mir`). The same C a
// TinyCC run compiles, the runtime included, is compiled instead by MIR's C
// front end (c2mir) and its optimizing generator (third_party/mir, MIT), into
// this process, and run there. Before generation, calls of sqrt and sqrtf
// become MIR's square root, an instruction added to it, which the generator
// keeps in registers rather than spilling every live one around a call into
// the C library (c2mir's own copies of small structs are moves, another
// change to it). memcpy stays a call: written as loads and stores, a copy
// out of a register variable whose address c2mir took read its stack home
// before the generator had stored it there. The runtime treats c2mir as it
// treats TinyCC where either lacks GCC's extensions (__MIRC__,
// src/runtime/runtime.h).

#pragma once

#include <csetjmp>
#include <cstdarg>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"
}

namespace goose {

struct MirReader {
    const char *pos;
};

inline int MirGetc(void *data) {
    auto r = (MirReader *)data;
    return *r->pos ? (int)(unsigned char)*r->pos++ : EOF;
}

// MIR_link's resolver takes no user data: the packages' native functions the
// run hands it, then the C library's.
inline const vector<pair<string, const void *>> *mir_symbols = nullptr;

inline void *MirResolve(const char *name) {
    if (mir_symbols)
        for (auto &[n, addr] : *mir_symbols)
            if (n == name) return (void *)addr;
    return SystemSymbol(name);
}

// MIR reports an error by calling this, which must not return: the run's
// compile is abandoned through a longjmp to MirCompile, which holds nothing
// a skipped destructor would leak.
inline jmp_buf *mir_bail = nullptr;
inline char mir_error[512];

[[noreturn]] inline void MirError(MIR_error_type_t, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(mir_error, sizeof mir_error, format, ap);
    va_end(ap);
    longjmp(*mir_bail, 1);
}

// Calls of sqrt and sqrtf, made MIR's square root.
inline void MirLowerCalls(MIR_context_t ctx) {
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (auto it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
            if (it->item_type != MIR_func_item) continue;
            auto f = it->u.func;
            for (auto insn = DLIST_HEAD(MIR_insn_t, f->insns), next = insn; insn; insn = next) {
                next = DLIST_NEXT(MIR_insn_t, insn);
                if (insn->code != MIR_CALL || insn->nops < 4) continue;
                if (insn->ops[0].mode != MIR_OP_REF || insn->ops[1].mode != MIR_OP_REF) continue;
                auto fn = insn->ops[1].u.ref, pr = insn->ops[0].u.ref;
                if (fn->item_type != MIR_import_item || pr->item_type != MIR_proto_item) continue;
                auto p = pr->u.proto;
                if (p->nres != 1 || p->vararg_p) continue;
                string_view name = fn->u.import_id;
                auto nargs = VARR_LENGTH(MIR_var_t, p->args);
                auto add = [&](MIR_insn_t ni) { MIR_insert_insn_before(ctx, it, insn, ni); };
                if (nargs == 1 && insn->nops == 4 &&
                    ((name == "sqrt" && p->res_types[0] == MIR_T_D) ||
                     (name == "sqrtf" && p->res_types[0] == MIR_T_F))) {
                    add(MIR_new_insn(ctx, name == "sqrt" ? MIR_DSQRT : MIR_FSQRT, insn->ops[2], insn->ops[3]));
                    MIR_remove_insn(ctx, it, insn);
                }
            }
        }
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)   // setjmp: nothing below it has a destructor to skip
#endif
// Compiles source and links it, returning its main or nullptr with the
// reason in mir_error; diagnostics from c2mir go to msgs. Plain C types only
// from the setjmp on (MirError's longjmp lands here).
inline void *MirCompile(MIR_context_t ctx, const char *source, const char *name, FILE *msgs,
                        c2mir_options *ops, int *c2mir_on, int *gen_on) {
    jmp_buf bail;
    MirReader reader;
    void *mainp = nullptr;
    mir_error[0] = 0;
    mir_bail = &bail;
    if (setjmp(bail)) {
        mir_bail = nullptr;
        return nullptr;
    }
    // MIR's callback type carries its own noreturn attribute, which clang
    // takes as part of the type: the function is one, by [[noreturn]].
    MIR_set_error_func(ctx, reinterpret_cast<MIR_error_func_t>(&MirError));
    c2mir_init(ctx);
    *c2mir_on = 1;
    ops->message_file = msgs;
    reader.pos = source;
    if (!c2mir_compile(ctx, ops, MirGetc, &reader, name, NULL)) {
        snprintf(mir_error, sizeof mir_error, "c2mir did not compile the generated C");
        mir_bail = nullptr;
        return nullptr;
    }
    MirLowerCalls(ctx);
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        MIR_load_module(ctx, m);
    if (mir_symbols)
        for (auto &[n, addr] : *mir_symbols) MIR_load_external(ctx, n.c_str(), (void *)addr);
    MIR_gen_init(ctx);
    *gen_on = 1;
    // Level 1, MIR's code selection and register allocation without the
    // global optimizations level 2 adds: at 2 a loop that appends through a
    // stack-top pointer kept it stale past the loop (codegen/
    // append_constructed_results), and the compare suite runs no slower
    // at 1 (3.53 s either way in WSL).
    MIR_gen_set_optimize_level(ctx, 1);
    // Each function generated as it is first called: the runtime's are many,
    // and a run calls few.
    MIR_link(ctx, MIR_set_lazy_gen_interface, MirResolve);
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m && !mainp; m = DLIST_NEXT(MIR_module_t, m))
        for (auto it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
            if (it->item_type == MIR_func_item && strcmp(MIR_item_name(ctx, it), "main") == 0) {
                mainp = it->addr;
                break;
            }
    if (!mainp) snprintf(mir_error, sizeof mir_error, "the generated C has no main");
    mir_bail = nullptr;
    return mainp;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// As RunJit (jit.h), through MIR: `source` is the C RunJit would compile,
// its resource-directory define included.
inline int RunMir(const string &source, const string &libpath, const string &progname,
                  const vector<string> &progargs, const vector<pair<string, const void *>> &symbols) {
    (void)libpath;
    #ifdef _WIN32
        // The runtime's Windows half needs <windows.h>, which no header set
        // here gives c2mir in a form it reads: the SDK's is MSVC's dialect,
        // and TinyCC's winnt.h implements its intrinsics in inline assembly.
        (void)source, (void)progname, (void)progargs, (void)symbols;
        throw CompileError { "gust run --mir is not available on Windows yet: MIR's C front end cannot read "
                             "the Windows headers the runtime needs; gust run (TinyCC) and gust build work" };
    #else
    vector<string> incdirs;
    #ifdef __APPLE__
        if (auto sdk = MacSdkPath(); !sdk.empty()) incdirs.push_back(cat(sdk, "/usr/include"));
    #endif
    vector<const char *> incs;
    for (auto &d : incdirs) incs.push_back(d.c_str());
    c2mir_options ops;
    memset(&ops, 0, sizeof ops);
    ops.ignore_warnings_p = 1;
    ops.include_dirs_num = incs.size();
    ops.include_dirs = incs.empty() ? nullptr : incs.data();
    auto msgs = tmpfile();
    auto ctx = MIR_init();
    int c2mir_on = 0, gen_on = 0;
    mir_symbols = &symbols;
    auto mainp = MirCompile(ctx, source.c_str(), progname.c_str(), msgs ? msgs : stderr, &ops, &c2mir_on, &gen_on);
    string diags;
    if (msgs) {
        fflush(msgs);
        rewind(msgs);
        char buf[1024];
        for (size_t n; (n = fread(buf, 1, sizeof buf, msgs)) > 0;) diags.append(buf, n);
        fclose(msgs);
    }
    if (!mainp) {
        auto msg = cat("mir: ", mir_error, "\n", diags);
        if (gen_on) MIR_gen_finish(ctx);
        if (c2mir_on) c2mir_finish(ctx);
        MIR_finish(ctx);
        mir_symbols = nullptr;
        throw CompileError { msg };
    }
    // main takes its arguments as C's does: an array of writable pointers.
    auto name = progname;
    auto args = progargs;
    vector<char *> argv;
    argv.push_back(name.data());
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    auto code = ((int (*)(int, char **))mainp)((int)argv.size() - 1, argv.data());
    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    mir_symbols = nullptr;
    return code;
    #endif
}

}  // namespace goose
