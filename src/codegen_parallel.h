// Gust: `parallel for` (spec §6.5) in C. The loop's body is a call of the
// nested function the parser made of it, which the checker proved
// race-free (typecheck_parallel.h). The calling thread fills a job (the
// range, the free variables' arguments every call of the function passes,
// its globals instance and a thunk), starts workers on it (gs_pfor_start),
// takes chunks of the range itself as the loop it is, and waits for the
// workers (gs_pfor_finish). A worker's thunk opens a block of data stacks of
// its own, takes chunks too, and calls the function directly. Where there
// are no threads (a JIT run) the calling thread takes every chunk, in order.

#pragma once

namespace goose {

inline void CodeGen::GenParallelFor(ForLoop *f, const string &lov, const string &hiv, const string &iv) {
    auto c = Is<Call>(f->body->stmts[0]);
    assert(c && c->spec && sinfo.count(c->spec));
    auto sp = c->spec;
    auto &ki = sinfo[sp];
    usesparallel = true;
    vector<string> fv;
    for (auto v : ki.freevars) EmitFvArg(c->FreeVarArg(v), fv);
    auto n = pforthunks.size();
    PforThunk pt { sp, cat("gs_pfj", n), cat("gs_pft", n), fv.size(), CT(sp->argtypes[0]) };
    pforthunks.push_back(pt);
    Append(protos, "typedef struct { gs_pfor_job h; void *fv[", fv.empty() ? 1 : fv.size(), "]; } ", pt.job,
           ";\nstatic void ", pt.thunk, "(uint8_t *p);\n");
    auto j = T();
    L(pt.job, " ", j, ";");
    L(j, ".h.lo = (int64_t)", lov, ";");
    L(j, ".h.hi = (int64_t)", hiv, ";");
    L(j, ".h.next = ", j, ".h.lo;");
    L(j, ".h.chunk = gs_pfor_chunk(", j, ".h.lo, ", j, ".h.hi);");
    L(j, ".h.gl = gs_gl;");
    L(j, ".h.thunk = ", pt.thunk, ";");
    for (size_t k = 0; k < fv.size(); k++) L(j, ".fv[", k, "] = (void *)(", fv[k], ");");
    L("gs_pfor_start(&", j, ".h);");
    // This thread's share: the loop as written, on the chunks it takes.
    auto a = T(), b = T(), k = T();
    L("for (int64_t ", a, "; (", a, " = gs_pfor_take(&", j, ".h)) >= 0;) {");
    ind++;
    L("int64_t ", b, " = ", a, " < ", j, ".h.hi - ", j, ".h.chunk ? ", a, " + ", j, ".h.chunk : ", j, ".h.hi;");
    L("for (int64_t ", k, " = ", a, "; ", k, " < ", b, "; ", k, "++) {");
    ind++;
    L(CT(f->vdef->type), " ", iv, " = (", CT(f->vdef->type), ")", k, ";");
    GenBlockInner(f->body, Dst {});
    ind--;
    L("}");
    ind--;
    L("}");
    L("gs_pfor_finish(&", j, ".h, ", LocArgs(f->line), ");");
}

// Each parallel for's worker thunk, once the stacks are counted. A worker
// of the pool opens its block of data stacks once, as many as the hungriest
// body takes from a base of 0, its callees' included; then for each job it
// reads the calling thread's globals and calls the body on the chunks it
// takes.
inline void CodeGen::EmitParallelThunks(int64_t &workerregions) {
    int64_t most = 0;
    for (auto &pt : pforthunks) {
        auto stacks = ProgramStacks({ pt.sp });
        CheckStackLimit("a parallel for's body", stacks, pt.sp->sf->line);
        most = std::max(most, stacks);
    }
    workerregions = std::max(workerregions, most);
    for (auto &pt : pforthunks) {
        auto &ki = sinfo[pt.sp];
        auto stacks = most;
        string args = cat("(", pt.ivct, ")k");
        for (size_t i = 0; i < pt.nfv; i++) Append(args, ", j->fv[", i, "]");
        if (ki.needssp) args += ", 0";
        Append(code, "static void ", pt.thunk, "(uint8_t *p) {\n",
               "    ", pt.job, " *j = *(", pt.job, " **)p;\n",
               "    if (!gs_stks) gs_stack_block(", stacks, ");\n",
               "    gs_gl = j->h.gl;\n",
               "    for (int64_t a; (a = gs_pfor_take(&j->h)) >= 0;) {\n",
               "        int64_t b = a < j->h.hi - j->h.chunk ? a + j->h.chunk : j->h.hi;\n",
               "        for (int64_t k = a; k < b; k++) ", ki.cname, "(", args, ");\n",
               "    }\n}\n\n");
    }
}

}  // namespace goose
