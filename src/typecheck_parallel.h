// Gust: `parallel for` (spec §6.5). The parser makes the loop's body a nested
// function of its counter, which the loop calls once per iteration and which
// workers call too (codegen). What this proves of that function, once it is
// checked, is that no two iterations race, so that running them at once
// does what running them in order does:
//
//  * it writes only its own locals, and the enclosing code's arrays at its
//    own index: `out[i]`, or a field of that element, where `out` is an
//    array the enclosing code owns (a reference or slice could be another
//    name for one an iteration reads);
//  * where it writes such an array it reads it only there too, and its
//    length;
//  * it writes no global, nor any other variable around the loop, nor
//    through a reference or slice that may point outside the body, and it
//    resizes nothing outside the body;
//  * what it calls does the same: a callee writes no global and none of the
//    enclosing code's variables, takes the enclosing code's storage for
//    writing only as `out[i]`, and calls no extern fn but the standard
//    library's, whose effects stay in their arguments;
//  * it spawns, waits for and messages no thread.
//
// Reads of anything else are free: nothing writes it while the loop runs.

#pragma once

namespace goose {

inline void TypeCheck::CheckParallelFor(ForLoop *f) {
    if (f->iterkind != IK_COUNT && f->iterkind != IK_RANGE)
        Error(f, "a parallel for counts, `parallel for i in n` or `in a..b`: index an array with i");
    auto c = f->body && !f->body->stmts.empty() ? Is<Call>(f->body->stmts[0]) : nullptr;
    if (!c || !c->spec || !c->spec->body || c->spec->params.empty()) return;
    auto sp = c->spec;
    auto iv = sp->params[0];
    // Outside the body: a global, or a variable another activation owns.
    auto outer = [&](VarDef *v) { return v && (v->isglobal || v->ownerspec != sp); };
    // A reference or slice variable that may point outside the body.
    auto escapes = [&](VarDef *v) {
        if (!v || !v->type || !IsRefOrSlice(v->type)) return false;
        if (v->ref.Unknown()) return true;
        for (auto &a : v->ref.alts)
            if (a.root && outer(a.root)) return true;
        return false;
    };
    // The variable a place starts at: `a` of `a`, `a.f`, `a[j]`, `a[j].f`.
    auto base = [](Node *n) -> VarDef * {
        for (;;) {
            if (auto id = Is<Ident>(n)) return id->vdef;
            if (auto d = Is<Dot>(n)) { n = d->obj; continue; }
            if (auto ix = Is<Index>(n)) { n = ix->obj; continue; }
            if (auto u = Is<Unary>(n)) { n = u->child; continue; }
            return nullptr;
        }
    };
    // The array whose own element the place is: X of `X[i]` or a field of
    // it, X an array the enclosing code owns.
    auto own = [&](Node *n) -> VarDef * {
        if (auto u = Is<Unary>(n); u && u->op == T_BITAND) n = u->child;
        while (auto d = Is<Dot>(n)) {
            if (!d->IsField()) return nullptr;
            n = d->obj;
        }
        auto ix = Is<Index>(n);
        if (!ix) return nullptr;
        auto a = Is<Ident>(ix->obj), i = Is<Ident>(ix->idx);
        if (!a || !i || i->vdef != iv || !a->vdef || a->vdef->isglobal || !outer(a->vdef)) return nullptr;
        auto t = a->vdef->type;
        return t && t->kind == TY_ARRAY ? a->vdef : nullptr;
    };
    auto fail = [&](Node *at, const string &why) { Error(at, cat("a parallel for's body ", why)); };
    // Whether a callee writes through its parameter j, a reference or a
    // slice it takes without const but may only read (a scene's spheres
    // handed to a tracer): what its body writes there, through a reference
    // of its own into it, or hands on to a callee that writes it, as a
    // fixpoint over the calls, which recursion needs.
    map<pair<FnSpec *, size_t>, bool> pwrites;
    map<pair<FnSpec *, size_t>, vector<pair<FnSpec *, size_t>>> passes;
    set<FnSpec *> scanned;
    auto mutparam = [](TypeExpr *pt) {
        return pt && !pt->cq &&
               ((pt->kind == TY_REF && !(pt->ref && pt->ref->sub && pt->ref->sub->cq)) ||
                (pt->kind == TY_SLICE && !(pt->sub && pt->sub->cq)));
    };
    function<void(FnSpec *)> scan = [&](FnSpec *k) {
        if (!k || !scanned.insert(k).second) return;
        for (size_t j = 0; j < k->params.size(); j++) {
            pwrites[{ k, j }] = k->sf && k->sf->isextern && j < k->argtypes.size() && mutparam(k->argtypes[j]);
            passes[{ k, j }];
        }
        if (!k->body) return;
        // A by-reference loop's element stands for the parameter the loop
        // runs over (`for &s in spheres`): written through, or handed on to
        // a callee that writes it, it writes that parameter; only read, not.
        map<VarDef *, Node *> elems;   // The element, and what its loop runs over.
        // The variable a place is storage of, through by-reference loops'
        // elements and the body's own references into them.
        auto storage = [&](Node *place) -> VarDef * {
            auto v = base(place);
            for (int hops = 0; v && hops < 64; hops++) {
                auto it = elems.find(v);
                if (it == elems.end())
                    for (auto &a : v->ref.alts)
                        if ((it = elems.find(a.root)) != elems.end()) break;
                if (it == elems.end()) break;
                v = base(it->second);
            }
            return v;
        };
        // The parameter a place names: itself, or a reference of the body's
        // own that points into it.
        auto param = [&](Node *place) -> int {
            auto v = storage(place);
            for (size_t j = 0; v && j < k->params.size(); j++) {
                if (v == k->params[j]) return (int)j;
                for (auto &a : v->ref.alts) if (a.root == k->params[j]) return (int)j;
            }
            return -1;
        };
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            auto direct = [&](Node *place) {
                if (auto j = param(place); j >= 0) {
                    pwrites[{ k, (size_t)j }] = true;
                } else if (auto v = storage(place); v && v->type && IsRefOrSlice(v->type) && v->ref.Unknown()) {
                    // Through a reference that may point anywhere: into any
                    // of them.
                    for (size_t m = 0; m < k->params.size(); m++)
                        if (m < k->argtypes.size() && mutparam(k->argtypes[m])) pwrites[{ k, m }] = true;
                }
            };
            if (auto a = Is<Assign>(n)) direct(a->lval);
            else if (auto inc = Is<IncDec>(n)) direct(inc->lval);
            else if (auto fl = Is<ForLoop>(n); fl && fl->byref && fl->vdef) elems[fl->vdef] = fl->iter;
            else if (auto cl = Is<Call>(n)) {
                auto an = cl->ArgNodes();
                if (cl->builtin >= 0) {
                    if ((BuiltinByKind(cl->builtin).flags & BF_WRITE) && !an.empty()) direct(an[0]);
                } else {
                    vector<FnSpec *> ts;
                    if (cl->spec) ts.push_back(cl->spec);
                    for (auto d : cl->dispatch) ts.push_back(d);
                    for (auto t : ts) {
                        scan(t);
                        for (size_t m = 0; m < an.size() && m < t->argtypes.size(); m++) {
                            if (!mutparam(t->argtypes[m])) continue;
                            if (auto j = param(an[m]); j >= 0) {
                                passes[{ k, (size_t)j }].push_back({ t, m });
                            } else if (auto v = storage(an[m]); v && v->type && IsRefOrSlice(v->type) &&
                                                                v->ref.Unknown()) {
                                for (size_t q = 0; q < k->params.size() && q < k->argtypes.size(); q++)
                                    if (mutparam(k->argtypes[q])) passes[{ k, q }].push_back({ t, m });
                            }
                        }
                    }
                }
            }
            RunChildren(n, walk);
        };
        walk(k->body);
    };
    auto writesparam = [&](FnSpec *k, size_t j) {
        scan(k);
        for (auto changed = true; changed;) {
            changed = false;
            for (auto &[from, tos] : passes)
                if (!pwrites[from])
                    for (auto &to : tos)
                        if (pwrites[to]) {
                            pwrites[from] = true;
                            changed = true;
                            break;
                        }
        }
        return pwrites[{ k, j }];
    };
    set<VarDef *> written;
    auto write = [&](Node *place, Node *at, const char *how) {
        auto v = base(place);
        if (!v) return;
        if (v->isglobal)
            fail(at, cat(how, " the global ", v->name, ", which every iteration shares"));
        if (outer(v)) {
            if (auto w = own(place); w && string_view(how) == "writes") {
                written.insert(w);
                return;
            }
            fail(at, cat(how, " ", v->name, ", which another iteration may also reach: it may write the "
                         "enclosing code's arrays only at its own index, as ", v->name, "[", iv->name, "]"));
        }
        if (escapes(v))
            fail(at, cat(how, " through ", v->name, ", which may point outside the body"));
    };
    // What a callee may do, by specialization: true where it writes what
    // more than one iteration may reach.
    map<FnSpec *, int> shared;   // 1 visiting or safe, 2 writes shared
    function<bool(FnSpec *, Node *)> callee = [&](FnSpec *k, Node *at) -> bool {
        auto &st = shared[k];
        if (st) return st == 2;
        st = 1;
        if (k->sf && k->sf->isextern) {
            auto fi = k->sf->line.fileidx;
            auto stdlib = fi >= 0 && fi < (int)ast.filepkg.size() &&
                          (ast.packages[ast.filepkg[fi]].stdlib || ast.packages[ast.filepkg[fi]].builtin >= 0);
            if (!stdlib) {
                fail(at, cat("calls the extern fn ", k->sf->name, ", whose effects Gust cannot see"));
                st = 2;
            }
            return st == 2;
        }
        if (!k->body) return false;
        // A variable outside this callee that is also outside the body.
        auto kouter = [&](VarDef *v) { return v && v->ownerspec != k && outer(v); };
        auto bad = false;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n || bad) return;
            Node *place = nullptr;
            if (auto a = Is<Assign>(n)) place = a->lval;
            else if (auto inc = Is<IncDec>(n)) place = inc->lval;
            else if (auto cl = Is<Call>(n)) {
                if (cl->builtin >= 0 && (BuiltinByKind(cl->builtin).flags & BF_WRITE)) {
                    auto an = cl->ArgNodes();
                    if (!an.empty()) place = an[0];
                }
                if (cl->builtin == B_THREAD_SPAWN || cl->builtin == B_THREAD_SPAWN_CONTAINED ||
                    cl->builtin == B_THREAD_WAIT || cl->builtin == B_QPUT || cl->builtin == B_QSEND)
                    bad = true;
                if (cl->spec && cl->spec != k && callee(cl->spec, at)) bad = true;
                for (auto d : cl->dispatch) if (d != k && callee(d, at)) bad = true;
            }
            if (place) {
                auto v = base(place);
                if (v && (v->isglobal || kouter(v))) bad = true;
            }
            RunChildren(n, walk);
        };
        walk(k->body);
        if (bad) {
            fail(at, cat("calls ", k->sf ? k->sf->name : string_view("a function"),
                         ", which writes a global or a variable around the loop"));
            st = 2;
        }
        return bad;
    };
    function<void(Node *)> writes = [&](Node *n) {
        if (!n) return;
        if (auto a = Is<Assign>(n)) write(a->lval, n, "writes");
        else if (auto inc = Is<IncDec>(n)) write(inc->lval, n, "writes");
        else if (auto fl = Is<ForLoop>(n); fl && fl->byref) write(fl->iter, n, "writes the elements of");
        else if (auto cl = Is<Call>(n)) {
            auto an = cl->ArgNodes();
            if (cl->builtin >= 0) {
                auto &d = BuiltinByKind(cl->builtin);
                if ((d.flags & BF_WRITE) && !an.empty())
                    write(an[0], n, (d.flags & BF_SHRINKS) || cl->builtin == B_PUSH || cl->builtin == B_APPEND
                                        ? "resizes" : "writes");
                if (cl->builtin == B_THREAD_SPAWN || cl->builtin == B_THREAD_SPAWN_CONTAINED ||
                    cl->builtin == B_THREAD_WAIT || cl->builtin == B_QPUT || cl->builtin == B_QGET ||
                    cl->builtin == B_QPOLL || cl->builtin == B_QSEND || cl->builtin == B_QRECV ||
                    cl->builtin == B_THREAD_SELF)
                    fail(n, "uses threads or queues, which the loop's own threads would share");
            } else if (cl->spec) {
                // What it is handed for writing: the enclosing code's storage
                // only as the iteration's own element.
                for (size_t j = 0; j < an.size() && j < cl->spec->argtypes.size(); j++)
                    if (mutparam(cl->spec->argtypes[j]) && writesparam(cl->spec, j))
                        write(an[j], an[j], "hands for writing");
                callee(cl->spec, n);
                for (auto d : cl->dispatch) callee(d, n);
            }
        }
        RunChildren(n, writes);
    };
    writes(sp->body);
    // Reads of what it writes: only at its own index, and the length.
    function<void(Node *, Node *)> reads = [&](Node *n, Node *parent) {
        if (!n) return;
        if (auto id = Is<Ident>(n); id && id->vdef) {
            auto v = id->vdef;
            if (written.count(v)) {
                auto ix = Is<Index>(parent);
                auto ii = ix ? Is<Ident>(ix->idx) : nullptr;
                auto d = Is<Dot>(parent);
                auto ok = (ix && ix->obj == n && ii && ii->vdef == iv) ||
                          (d && d->obj == n && (d->member == B_LEN || d->member == B_CAP));
                if (!ok)
                    fail(n, cat("reads ", v->name, " other than at ", v->name, "[", iv->name, "], where "
                                "another iteration writes"));
            } else if (v->type && IsRefOrSlice(v->type) && !written.empty()) {
                auto may = v->ref.Unknown();
                for (auto &a : v->ref.alts) may = may || (a.root && written.count(a.root));
                if (may)
                    fail(n, cat("reads ", v->name, ", which may name an array it writes"));
            }
        }
        RunChildren(n, [&](Node *ch) { reads(ch, n); });
    };
    reads(sp->body, nullptr);
    sp->parallelbody = true;
}

}  // namespace goose
