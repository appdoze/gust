// Gust: the MIR backend for in-process runs (`gust run --mir`). The
// program's own C, as a build with a separate runtime has it, is compiled by
// MIR's C front end (c2mir) and its optimizing generator (third_party/mir,
// MIT) into this process, and run there against the runtime compiled into
// the compiler (src/runtime_native.c), which MIR finds by name. So c2mir
// reads no runtime and no system header but the C library's, and nothing
// is compiled for the runtime as a run starts. Before generation, calls of sqrt and sqrtf
// become MIR's square root, an instruction added to it, which the generator
// keeps in registers rather than spilling every live one around a call into
// the C library (c2mir's own copies of small structs are moves, another
// change to it). A memset or memcpy of up to 32 or 16 bytes, the runtime's
// small zeroing, copies and unaligned loads, becomes stores and loads there
// too (MirLowerCalls), but not through an address of a register variable
// that anything else reads: written in the runtime's C as loads and stores,
// such a copy had read the variable's stack home before the generator
// stored it there. The places of the frame c2mir
// keeps struct locals in become registers where nothing else can reach
// them (MirPromoteFrame), as a C compiler keeps a struct's fields in
// registers. The runtime treats c2mir as it
// treats TinyCC where either lacks GCC's extensions (__MIRC__,
// src/runtime/runtime.h).

#pragma once

#include <csetjmp>
#include <cstdarg>

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"
// The resource directory of the compiler's own runtime (runtime_os.h).
extern const char *gs_native_resource_dir;
// c2mir's report of each function's frame objects (MirPromoteFrame).
extern void (*gs_c2mir_frame_object)(const char *func, size_t offset, size_t size, int array_p);
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
// run hands it, then the runtime's and the C library's.
inline const vector<pair<string, const void *>> *mir_symbols = nullptr;

inline void *MirResolve(const char *name) {
    if (mir_symbols)
        for (auto &[n, addr] : *mir_symbols)
            if (n == name) return (void *)addr;
    // The program's unit calls the runtime's start by the version of the
    // runtime's text it was written with, the text this compiler's own
    // runtime was built from.
    if (!strncmp(name, "gs_rt_start_", 12)) name = "gs_rt_start";
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

// The registers of a function that may hold an address `addr` took of a
// register variable, or one computed from such an address.
inline unordered_set<MIR_reg_t> MirAddrRegs(MIR_context_t ctx, MIR_func_t f) {
    unordered_set<MIR_reg_t> regs;
    for (auto changed = true; changed;) {
        changed = false;
        for (auto insn = DLIST_HEAD(MIR_insn_t, f->insns); insn; insn = DLIST_NEXT(MIR_insn_t, insn)) {
            auto from = MIR_addr_code_p(insn->code);
            for (size_t i = 0; i < insn->nops && !from; i++) {
                int out;
                MIR_insn_op_mode(ctx, insn, i, &out);
                auto &o = insn->ops[i];
                if (out) continue;
                if (o.mode == MIR_OP_REG) from = regs.count(o.u.reg) > 0;
                else if (o.mode == MIR_OP_MEM) from = regs.count(o.u.mem.base) || regs.count(o.u.mem.index);
            }
            if (!from) continue;
            for (size_t i = 0; i < insn->nops; i++) {
                int out;
                MIR_insn_op_mode(ctx, insn, i, &out);
                if (out && insn->ops[i].mode == MIR_OP_REG) changed = regs.insert(insn->ops[i].u.reg).second || changed;
            }
        }
    }
    return regs;
}

// The instruction that last sets reg before insn in its block, or null
// (a label, or the function's start, comes first).
inline MIR_insn_t MirDefAt(MIR_context_t ctx, MIR_insn_t insn, MIR_reg_t reg) {
    for (auto p = DLIST_PREV(MIR_insn_t, insn); p && p->code != MIR_LABEL; p = DLIST_PREV(MIR_insn_t, p))
        for (size_t i = 0; i < p->nops; i++) {
            int out;
            MIR_insn_op_mode(ctx, p, i, &out);
            if (out && p->ops[i].mode == MIR_OP_REG && p->ops[i].u.reg == reg) return p;
        }
    return nullptr;
}

// The constant an operand of insn is: an immediate, or a register a
// `mov` of one set last in the block.
inline bool MirConstAt(MIR_context_t ctx, MIR_insn_t insn, const MIR_op_t &o, int64_t &v) {
    auto imm = [&](const MIR_op_t &x) {
        if (x.mode != MIR_OP_INT && x.mode != MIR_OP_UINT) return false;
        v = x.u.i;
        return true;
    };
    if (imm(o)) return true;
    if (o.mode != MIR_OP_REG) return false;
    auto d = MirDefAt(ctx, insn, o.u.reg);
    return d && d->code == MIR_MOV && imm(d->ops[1]);
}

// The `addr reg, v` (or `addr32`, a 32-bit variable's, at its start on the
// little-endian targets MIR has) that set reg last in insn's block, and v.
inline MIR_insn_t MirAddrAt(MIR_context_t ctx, MIR_insn_t insn, MIR_reg_t reg, MIR_reg_t &v) {
    auto d = MirDefAt(ctx, insn, reg);
    if (!d || (d->code != MIR_ADDR && d->code != MIR_ADDR32) || d->ops[1].mode != MIR_OP_REG) return nullptr;
    v = d->ops[1].u.reg;
    return d;
}

// Whether an instruction of f other than `except` reads reg.
inline bool MirReads(MIR_context_t ctx, MIR_func_t f, MIR_reg_t reg, MIR_insn_t except = nullptr) {
    for (auto i = DLIST_HEAD(MIR_insn_t, f->insns); i; i = DLIST_NEXT(MIR_insn_t, i))
        for (size_t k = 0; k < i->nops && i != except; k++) {
            int out;
            MIR_insn_op_mode(ctx, i, k, &out);
            auto &o = i->ops[k];
            if (!out && ((o.mode == MIR_OP_REG && o.u.reg == reg) ||
                         (o.mode == MIR_OP_MEM && (o.u.mem.base == reg || o.u.mem.index == reg))))
                return true;
        }
    return false;
}

// Calls of sqrt and sqrtf, made MIR's square root, and of a small memset or
// memcpy, made stores and loads.
inline void MirLowerCalls(MIR_context_t ctx) {
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (auto it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
            if (it->item_type != MIR_func_item) continue;
            auto f = it->u.func;
            auto addrregs = MirAddrRegs(ctx, f);
            vector<MIR_insn_t> addrs;
            int mcregs = 0;
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
                    continue;
                }
                if (nargs != 3 || insn->nops != 6 || insn->ops[2].mode != MIR_OP_REG ||
                    insn->ops[3].mode != MIR_OP_REG || (name != "memset" && name != "memcpy"))
                    continue;
                // Small memsets and memcpys of a constant size, where the
                // runtime's GS_MEMZERO, GS_MEMCPY, gs_ld32 and gs_ld64 call
                // them (runtime.h): stores and loads where the call was,
                // whose alias set is 0, any, so no type-based alias analysis
                // can move them past the stores and loads they used to be
                // ordered with by the call. A store of zero for each part of a
                // pushed element (bintrees' links), a load or a store of the
                // register a copy into or out of `&v` names (a slice key's
                // compare), a load and a store for the rest. Otherwise an
                // address `addr` took is left to the call: a store through
                // it of a narrower type than the variable's left the
                // generator's address transformation reading stale halves
                // (typing/explicit_type_args crashed).
                int64_t n = 0, c = -1;
                if (!MirConstAt(ctx, insn, insn->ops[5], n) || n <= 0 || n > 32) continue;
                auto dr = insn->ops[3].u.reg;
                auto part = [](int64_t left) { return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1; };
                auto ptype = [](int64_t w) {
                    return w == 8 ? MIR_T_I64 : w == 4 ? MIR_T_U32 : w == 2 ? MIR_T_U16 : MIR_T_U8;
                };
                auto mem = [&](MIR_type_t t, int64_t off, MIR_reg_t base) {
                    return MIR_new_mem_op(ctx, t, off, base, 0, 1);
                };
                // The call's result, its first argument, where anything reads it.
                auto done = [&]() {
                    if (MirReads(ctx, f, insn->ops[2].u.reg))
                        add(MIR_new_insn(ctx, MIR_MOV, insn->ops[2], insn->ops[3]));
                    MIR_remove_insn(ctx, it, insn);
                };
                if (name == "memset") {
                    if (!MirConstAt(ctx, insn, insn->ops[4], c) || c != 0) continue;
                    // Zeroing the whole of a register variable through its
                    // address, which nothing else reads: a mov of zero.
                    MIR_reg_t zv = 0;
                    if (auto za = MirAddrAt(ctx, insn, dr, zv)) {
                        if (MirReads(ctx, f, insn->ops[2].u.reg) || MirReads(ctx, f, dr, insn)) continue;
                        auto rt = MIR_reg_type(ctx, zv, f);
                        auto wide = za->code == MIR_ADDR;
                        MIR_insn_t z = nullptr;
                        if (rt == MIR_T_I64 && n == (wide ? 8 : 4))
                            z = MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, zv), MIR_new_int_op(ctx, 0));
                        else if (wide && rt == MIR_T_D && n == 8)
                            z = MIR_new_insn(ctx, MIR_DMOV, MIR_new_reg_op(ctx, zv), MIR_new_double_op(ctx, 0.0));
                        else if (wide && rt == MIR_T_F && n == 4)
                            z = MIR_new_insn(ctx, MIR_FMOV, MIR_new_reg_op(ctx, zv), MIR_new_float_op(ctx, 0.0f));
                        if (!z) continue;
                        add(z);
                        addrs.push_back(za);
                        MIR_remove_insn(ctx, it, insn);
                        continue;
                    }
                    if (addrregs.count(dr)) continue;
                    for (int64_t off = 0; off < n; off += part(n - off))
                        add(MIR_new_insn(ctx, MIR_MOV, mem(ptype(part(n - off)), off, dr), MIR_new_int_op(ctx, 0)));
                    done();
                    continue;
                }
                if (insn->ops[4].mode != MIR_OP_REG) continue;
                auto sr = insn->ops[4].u.reg;
                MIR_reg_t dv = 0, sv = 0;
                auto da = MirAddrAt(ctx, insn, dr, dv), sa = MirAddrAt(ctx, insn, sr, sv);
                // A variable whose address something else reads stays the
                // call's.
                if ((da || sa) && (MirReads(ctx, f, insn->ops[2].u.reg) ||
                                   (da && MirReads(ctx, f, dr, insn)) || (sa && MirReads(ctx, f, sr, insn))))
                    continue;
                // The mov of n bytes to or from a register variable.
                // The copy covers the whole variable: all 8 bytes of a
                // register `addr` took, the 4 of a float's, or the 4 of a
                // 32-bit variable's `addr32` (whose upper half c2mir keeps
                // extended).
                auto regmov = [&](MIR_insn_t a, MIR_reg_t v, bool load, MIR_insn_code_t &code, MIR_type_t &t) {
                    auto rt = MIR_reg_type(ctx, v, f);
                    auto wide = a->code == MIR_ADDR;
                    if (wide && rt == MIR_T_D && n == 8) return code = MIR_DMOV, t = MIR_T_D, true;
                    if (wide && rt == MIR_T_F && n == 4) return code = MIR_FMOV, t = MIR_T_F, true;
                    if (rt != MIR_T_I64 || n != (wide ? 8 : 4)) return false;
                    code = MIR_MOV;
                    t = n == 8 ? MIR_T_I64 : MIR_T_U32;
                    // A load into a 32-bit variable extends as c2mir's name
                    // for it says: `u` unsigned, `i` signed.
                    auto nm = MIR_reg_name(ctx, v, f);
                    if (load && n == 4) {
                        if (nm[0] == 'i') t = MIR_T_I32;
                        else if (nm[0] != 'u') return false;
                    }
                    return true;
                };
                MIR_insn_code_t code;
                MIR_type_t t;
                if (da && !sa && !addrregs.count(sr) && regmov(da, dv, true, code, t)) {
                    add(MIR_new_insn(ctx, code, MIR_new_reg_op(ctx, dv), mem(t, 0, sr)));
                    addrs.push_back(da);
                    done();
                } else if (sa && !da && !addrregs.count(dr) && regmov(sa, sv, false, code, t)) {
                    add(MIR_new_insn(ctx, code, mem(t, 0, dr), MIR_new_reg_op(ctx, sv)));
                    addrs.push_back(sa);
                    done();
                } else if (!da && !sa && !addrregs.count(dr) && !addrregs.count(sr) && n <= 16) {
                    // Every part loaded before any is stored, as memmove
                    // would; memcpy's runs do not overlap anyway.
                    vector<pair<MIR_reg_t, int64_t>> parts;
                    for (int64_t off = 0; off < n; off += part(n - off)) {
                        auto r = MIR_new_func_reg(ctx, f, MIR_T_I64, cat("gs_mc", mcregs++).c_str());
                        add(MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r), mem(ptype(part(n - off)), off, sr)));
                        parts.push_back({ r, off });
                    }
                    for (auto [r, off] : parts)
                        add(MIR_new_insn(ctx, MIR_MOV, mem(ptype(part(n - off)), off, dr), MIR_new_reg_op(ctx, r)));
                    done();
                }
            }
            // An `addr` whose address only the calls lowered took is dead:
            // without it the variable is a register again.
            sort(addrs.begin(), addrs.end());
            addrs.erase(unique(addrs.begin(), addrs.end()), addrs.end());
            for (auto a : addrs)
                if (!MirReads(ctx, f, a->ops[0].u.reg)) MIR_remove_insn(ctx, it, a);
        }
}

// A frame object c2mir reported: a local that is not a register, at its
// offset from fp, and whether it holds an array.
struct MirFrameObject { int64_t off, size; bool array; };
inline unordered_map<string, vector<MirFrameObject>> mir_frames;

inline void MirFrameObjectSeen(const char *func, size_t offset, size_t size, int array_p) {
    mir_frames[func].push_back({ (int64_t)offset, (int64_t)size, array_p != 0 });
}

// c2mir places a function's struct locals in its frame, one alloca (its
// register `fp`), and reaches them through addresses `add t, fp, k`: a
// field read is a load, a struct copy 8-byte moves. The generator keeps
// none of that in registers, which leaves vector arithmetic such as
// n-body's a store and a load for every use. Where a place in the frame is
// read and written only whole, as one 8-byte integer or double, at
// addresses that say which place they are, it is made a register: the
// frame is reachable through fp alone, so nothing can tell.
//
// An address is fp plus constants, which c2mir computes into registers it
// reuses, so each use is resolved from the definitions before it in its
// block, back to the last label: in the C c2mir writes, a constant is
// moved into a register just before its use. An address leaving the
// accesses gives away what it can reach: a memcpy or memset of a known
// size what it covers, anything else the object it points into (c2mir
// reports each, gs_c2mir_frame_object), and fp plus an index the arrays,
// as in C an index stays within its array. One that cannot be resolved
// leaves the frame as it is. A struct copy's 8-byte integer moves into and
// out of a place that holds a double become moves of doubles.
inline void MirPromoteFrame(MIR_context_t ctx, MIR_item_t it) {
    auto f = it->u.func;
    auto ops = [&](MIR_insn_t insn) { return MIR_insn_nops(ctx, insn); };
    MIR_reg_t fp = 0;
    for (auto insn = DLIST_HEAD(MIR_insn_t, f->insns); insn && !fp; insn = DLIST_NEXT(MIR_insn_t, insn))
        if (insn->code == MIR_ALLOCA && insn->ops[0].mode == MIR_OP_REG &&
            string_view(MIR_reg_name(ctx, insn->ops[0].u.reg, f)) == "fp")
            fp = insn->ops[0].u.reg;
    if (!fp) return;
    auto out = [&](MIR_insn_t insn, size_t i) {
        int o = 0;
        MIR_insn_op_mode(ctx, insn, i, &o);
        return o != 0;
    };
    // Every register's definitions, and which can hold a frame address.
    unordered_map<MIR_reg_t, vector<MIR_insn_t>> defs;
    for (auto insn = DLIST_HEAD(MIR_insn_t, f->insns); insn; insn = DLIST_NEXT(MIR_insn_t, insn))
        for (size_t i = 0; i < ops(insn); i++)
            if (out(insn, i) && insn->ops[i].mode == MIR_OP_REG) defs[insn->ops[i].u.reg].push_back(insn);
    if (defs[fp].size() != 1) return;
    unordered_set<MIR_reg_t> framish { fp };
    for (auto changed = true; changed;) {
        changed = false;
        for (auto &[r, ds] : defs) {
            if (framish.count(r)) continue;
            // Only a move or an addition derives an address; whatever else
            // takes one uses it as a value, which gives away its object
            // where it does, and C keeps the result within that object.
            auto from = false;
            for (auto d : ds)
                if (d->code == MIR_MOV || d->code == MIR_ADD)
                    for (size_t i = 1; i < ops(d); i++)
                        from = from || (d->ops[i].mode == MIR_OP_REG && framish.count(d->ops[i].u.reg));
            if (from) {
                framish.insert(r);
                changed = true;
            }
        }
    }
    // The definition of r that reaches `at`, within at's block.
    auto reaching = [&](MIR_reg_t r, MIR_insn_t at) -> MIR_insn_t {
        for (auto p = DLIST_PREV(MIR_insn_t, at); p; p = DLIST_PREV(MIR_insn_t, p)) {
            if (p->code == MIR_LABEL) return nullptr;
            for (size_t i = 0; i < ops(p); i++)
                if (out(p, i) && p->ops[i].mode == MIR_OP_REG && p->ops[i].u.reg == r) return p;
        }
        return nullptr;
    };
    auto constant = [&](const MIR_op_t &o, MIR_insn_t at, int64_t &v) {
        if (o.mode == MIR_OP_INT || o.mode == MIR_OP_UINT) {
            v = o.u.i;
            return true;
        }
        if (o.mode != MIR_OP_REG) return false;
        auto d = reaching(o.u.reg, at);
        if (!d || d->code != MIR_MOV || (d->ops[1].mode != MIR_OP_INT && d->ops[1].mode != MIR_OP_UINT))
            return false;
        v = d->ops[1].u.i;
        return true;
    };
    // Whether o is an index scaled to its elements, as c2mir writes one --
    // a product or a shift by a constant, plus constants (into c) -- and so
    // added to an array's address, never a distance to another object.
    function<bool(const MIR_op_t &, MIR_insn_t, int64_t &, int)> scaled = [&](const MIR_op_t &o, MIR_insn_t at,
                                                                             int64_t &c, int depth) -> bool {
        if (o.mode != MIR_OP_REG || depth > 16) return false;
        auto d = reaching(o.u.reg, at);
        if (!d) return false;
        int64_t k = 0;
        switch (d->code) {
            case MIR_MOV: return d->ops[1].mode == MIR_OP_REG && scaled(d->ops[1], d, c, depth + 1);
            case MIR_MUL: case MIR_MULS:
                return constant(d->ops[1], d, k) || constant(d->ops[2], d, k);
            case MIR_LSH: case MIR_LSHS: return constant(d->ops[2], d, k);
            case MIR_ADD: case MIR_ADDS:
                if (constant(d->ops[2], d, k) && scaled(d->ops[1], d, c, depth + 1)) { c += k; return true; }
                if (constant(d->ops[1], d, k) && scaled(d->ops[2], d, c, depth + 1)) { c += k; return true; }
                return false;
            default: return false;
        }
    };
    // What a register holds at `at`: no frame address, fp + k, fp + k plus
    // an index, or a frame address of no known kind.
    enum Kind { NOTFRAME, ADDR, INDEXED, UNKNOWN };
    struct Res { Kind kind; int64_t off; };
    function<Res(const MIR_op_t &, MIR_insn_t, int)> resolve = [&](const MIR_op_t &o, MIR_insn_t at,
                                                                     int depth) -> Res {
        if (o.mode != MIR_OP_REG || !framish.count(o.u.reg)) return { NOTFRAME, 0 };
        if (o.u.reg == fp) return { ADDR, 0 };
        auto d = depth < 64 ? reaching(o.u.reg, at) : nullptr;
        if (!d) return { UNKNOWN, 0 };
        if (d->code == MIR_MOV && d->ops[1].mode == MIR_OP_REG) return resolve(d->ops[1], d, depth + 1);
        // Not derived here (another operation's result, its frame address
        // given away where it was taken).
        if (d->code != MIR_ADD) return { NOTFRAME, 0 };
        auto a = resolve(d->ops[1], d, depth + 1), b = resolve(d->ops[2], d, depth + 1);
        const MIR_op_t *other = &d->ops[2];
        if (a.kind == NOTFRAME) {
            swap(a, b);
            other = &d->ops[1];
        }
        if (a.kind == NOTFRAME) return { NOTFRAME, 0 };
        if (a.kind == UNKNOWN || b.kind != NOTFRAME) return { UNKNOWN, 0 };
        int64_t c = 0;
        if (constant(*other, d, c)) return { a.kind, a.off + c };
        if (scaled(*other, d, c, 0)) return { INDEXED, a.off + c };
        return { UNKNOWN, 0 };
    };
    vector<MirFrameObject> objs;
    if (auto o = mir_frames.find(f->name); o != mir_frames.end()) objs = o->second;
    // The places accessed, and the ranges of the frame given away.
    struct Access { MIR_insn_t insn; size_t op; int64_t off; MIR_type_t type; bool copy; };
    vector<Access> accesses;
    vector<pair<int64_t, int64_t>> away;
    auto giveobject = [&](int64_t k) {
        auto found = false;
        for (auto &o : objs)
            if (o.off <= k && k < o.off + o.size) {
                away.push_back({ o.off, o.size });
                found = true;
            }
        if (!found) away.push_back({ k, INT64_MAX / 4 });
    };
    auto givearrays = [&] {
        if (objs.empty()) away.push_back({ INT64_MIN / 4, INT64_MAX / 2 });
        for (auto &o : objs)
            if (o.array) away.push_back({ o.off, o.size });
    };
    // Gives away what a resolved frame address reaches; false where it
    // cannot say.
    auto give = [&](Res r, int64_t disp) {
        if (r.kind == UNKNOWN) return false;
        if (r.kind == INDEXED) givearrays();
        if (r.kind != NOTFRAME) giveobject(r.off + disp);
        return true;
    };
    for (auto insn = DLIST_HEAD(MIR_insn_t, f->insns); insn; insn = DLIST_NEXT(MIR_insn_t, insn)) {
        auto call = MIR_call_code_p(insn->code);
        size_t first = 0;
        string_view name;
        int64_t n = 0;
        auto known = false;
        if (call) {
            auto fn = insn->ops[1].mode == MIR_OP_REF ? insn->ops[1].u.ref : nullptr;
            name = fn && fn->item_type == MIR_import_item ? fn->u.import_id : "";
            first = 2 + (insn->ops[0].mode == MIR_OP_REF ? insn->ops[0].u.ref->u.proto->nres : 0);
            known = (name == "memcpy" || name == "memmove" || name == "memset") && ops(insn) == first + 3 &&
                    constant(insn->ops[first + 2], insn, n);
        }
        // An address only computed into a register: what it reaches is
        // decided where that register is used.
        auto derives = !call && (insn->code == MIR_ADD || insn->code == MIR_MOV) &&
                       insn->ops[0].mode == MIR_OP_REG;
        auto copy = insn->code == MIR_MOV && insn->ops[0].mode == MIR_OP_MEM && insn->ops[1].mode == MIR_OP_MEM;
        for (size_t i = call ? 2 : 0; i < ops(insn); i++) {
            auto &o = insn->ops[i];
            if (o.mode == MIR_OP_MEM) {
                MIR_op_t idx = o, base = o;
                idx.mode = base.mode = MIR_OP_REG;
                idx.u.reg = o.u.mem.index;
                base.u.reg = o.u.mem.base;
                auto ri = o.u.mem.index ? resolve(idx, insn, 0) : Res { NOTFRAME, 0 };
                if (ri.kind != NOTFRAME) return;
                auto rb = o.u.mem.base ? resolve(base, insn, 0) : Res { NOTFRAME, 0 };
                if (rb.kind == NOTFRAME) continue;
                if (call && MIR_all_blk_type_p(o.u.mem.type)) {
                    // A block a call takes at its address, the base; a
                    // block's disp is its size.
                    if (!give(rb, 0)) return;
                    away.push_back({ rb.off, o.u.mem.disp });
                    continue;
                }
                if (rb.kind == ADDR && !o.u.mem.index) {
                    accesses.push_back({ insn, i, rb.off + o.u.mem.disp, o.u.mem.type, copy });
                    continue;
                }
                // Indexed.
                if (rb.kind == ADDR && o.u.mem.index) rb.kind = INDEXED;
                if (!give(rb, o.u.mem.disp)) return;
            } else if (o.mode == MIR_OP_REG && !out(insn, i)) {
                if (derives) continue;
                auto r = resolve(o, insn, 0);
                if (r.kind == NOTFRAME) continue;
                if (known && r.kind == ADDR && i < first + 2 && (name != "memset" || i == first)) {
                    away.push_back({ r.off, n });
                    continue;
                }
                // An address used as a value: stored, compared, passed.
                if (!give(r, 0)) return;
            }
        }
    }
    // The places: 8 bytes each, of one type, overlapping no other and given
    // away nowhere. A struct copy's moves agree with the type of what they
    // move; a place only ever copied is an integer.
    struct Place { int64_t size = 0; MIR_type_t type = MIR_T_UNDEF; bool bad = false; MIR_reg_t reg = 0; };
    map<int64_t, Place> places;
    for (auto &a : accesses) {
        auto &p = places[a.off];
        auto sz = (int64_t)_MIR_type_size(ctx, a.type);
        if ((p.size && p.size != sz) || sz != 8) p.bad = true;
        p.size = sz;
        if (a.copy) continue;
        auto t = a.type == MIR_T_D ? MIR_T_D
                 : a.type == MIR_T_I64 || a.type == MIR_T_U64 || a.type == MIR_T_P ? MIR_T_I64
                                                                                   : MIR_T_UNDEF;
        if (t == MIR_T_UNDEF || (p.type != MIR_T_UNDEF && p.type != t)) p.bad = true;
        p.type = t;
    }
    int64_t end = INT64_MIN;
    Place *last = nullptr;
    for (auto &[off, p] : places) {
        if (last && off < end) last->bad = p.bad = true;
        for (auto &[a, len] : away)
            if (off < a + len && a < off + p.size) p.bad = true;
        if (off + p.size > end) {
            end = off + p.size;
            last = &p;
        }
    }
    // A copy between two places makes them one type; one between a place
    // and other memory reads or writes that memory as the place's type.
    unordered_map<MIR_insn_t, vector<size_t>> ofinsn;
    for (size_t k = 0; k < accesses.size(); k++) ofinsn[accesses[k].insn].push_back(k);
    for (auto changed = true; changed;) {
        changed = false;
        for (auto &[insn, ks] : ofinsn) {
            if (ks.size() != 2 || !accesses[ks[0]].copy) continue;
            auto &d = places[accesses[ks[0]].off], &e = places[accesses[ks[1]].off];
            if (d.bad || e.bad || d.type == e.type) continue;
            if (d.type != MIR_T_UNDEF && e.type != MIR_T_UNDEF) d.bad = e.bad = true;
            else d.type = e.type = d.type != MIR_T_UNDEF ? d.type : e.type;
            changed = true;
        }
    }
    auto count = 0;
    for (auto &[off, p] : places) {
        if (p.bad) continue;
        if (p.type == MIR_T_UNDEF) p.type = MIR_T_I64;
        char name[40];
        snprintf(name, sizeof name, "gs_place_%d", count++);
        p.reg = MIR_new_func_reg(ctx, f, p.type, name);
    }
    // Each access of a place made one of its register, and a copy into or
    // out of it a move of its type.
    for (auto &a : accesses) {
        auto &p = places[a.off];
        if (p.bad) continue;
        if (a.copy && p.type == MIR_T_D) {
            a.insn->code = MIR_DMOV;
            auto &other = a.insn->ops[1 - a.op];
            if (other.mode == MIR_OP_MEM) other.u.mem.type = MIR_T_D;
        }
        a.insn->ops[a.op] = MIR_new_reg_op(ctx, p.reg);
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
    mir_frames.clear();
    gs_c2mir_frame_object = MirFrameObjectSeen;
    if (!c2mir_compile(ctx, ops, MirGetc, &reader, name, NULL)) {
        snprintf(mir_error, sizeof mir_error, "c2mir did not compile the generated C");
        mir_bail = nullptr;
        return nullptr;
    }
    MirLowerCalls(ctx);
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (auto it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
            if (it->item_type == MIR_func_item) MirPromoteFrame(ctx, it);
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        MIR_load_module(ctx, m);
    if (mir_symbols)
        for (auto &[n, addr] : *mir_symbols) MIR_load_external(ctx, n.c_str(), (void *)addr);
    MIR_gen_init(ctx);
    *gen_on = 1;
    // Level 2: MIR's global value numbering, copy propagation, dead store
    // elimination and loop-invariant motion over the registers
    // MirPromoteFrame made of the frame's struct fields. In WSL the compare
    // suite runs 1.54x off a native gcc build rather than level 1's 1.94x
    // (n-body 0.24 s rather than 0.50).
    MIR_gen_set_optimize_level(ctx, 2);
    // Every function generated now, the program's own being all there is:
    // generating each as it is first called saved 6 ms of a 26 ms run in
    // WSL, and on Windows MIR's thunk for it corrupted the first call.
    MIR_link(ctx, MIR_set_gen_interface, MirResolve);
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

// As RunJit (jit.h), through MIR: `source` is the program's C written for a
// separate runtime, which the compiler's own is.
inline int RunMir(const string &source, const string &libpath, const string &progname,
                  const vector<string> &progargs, const vector<pair<string, const void *>> &symbols) {
    vector<string> incdirs;
    #ifdef _WIN32
        // The C library's headers as TinyCC has them, which c2mir reads
        // where the SDK's MSVC dialect it does not.
        incdirs.push_back(cat(libpath, "/include"));
    #elif defined(__APPLE__)
        (void)libpath;
        if (auto sdk = MacSdkPath(); !sdk.empty()) incdirs.push_back(cat(sdk, "/usr/include"));
    #else
        (void)libpath;
    #endif
    static string resources;
    resources = JitResourceDir(progname);
    gs_native_resource_dir = resources.c_str();
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
}

}  // namespace goose
