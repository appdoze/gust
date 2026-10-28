// Goose compiler — base-case inlining for self-recursive functions, a pass of
// the optimizer (optimize.h) that runs inside its walk: the driver sets the
// pass up once per specialization, and the Call node's Opt offers it every
// call it did not inline.
//
// Cycle members never inline, so the leaf call of a recursive tree walk is
// always a real frame. Where a self-recursive body *starts* with
// `if c { return e; }` — or with `if c { ... } else { return e; }`, the same
// test negated, as `guard c else { return e; }` parses — and both c and e
// read nothing but the parameters, globals and their own bindings, every
// direct self-call `f(a...)` (or `a1.f(a2...)`) is rewritten to
//
//     { let p1 = a1; ...; if c[p := t] { e[p := t] } else { f(t...) } }
//
// which removes half the calls of a complete tree walk. Both arms construct
// into the call's own destination (the ordinary rule for `if` as a value), so
// reference and non-fixed results need no special case -- except that an
// array result passed where a slice is expected has no destination an arm
// could build it in, and such a call is left alone. Binding the arguments
// first keeps them evaluated once and in order; c is the exception — the
// recursing arm runs it a second time inside the callee — which is why it is
// restricted to pure reads. The bindings are made in the scope around the
// block, as the call's arguments were (`inline_arg`): a slice argument may
// view a temporary, which the block's value can still view.
//
// The guarded form is the shape the other languages' tree builders take:
// `s1...; if c { ...self-calls... } s3...; return e;`, every self-call inside
// the `if`, which has no else. Where c does not hold, the body is s1, s3 and
// e, so a self-call becomes
//
//     { let p1 = a1; ...; if c[p := t] { f(t...) } else { s1; s3; e }[p := t] }
//
// For that c may only compare primitive scalar parameters the body never assigns, and
// literals: the call site evaluates it before s1 runs, where the callee
// evaluates it after, so it has to give the same answer then and be unable
// to abort. s1, s3 and e may not return, jump out or call into the cycle.
//
// Gust: the match form, a body that is one `match` on the parameters over
// an enum's variants, as a tree's evaluator is. Its leaf arms, those that do
// not recurse, are the base case: a self-call becomes
//
//     { let p1 = a1; ...; match s[p := t] { leaf arms[p := t], _ => f(t...) } }
//
// so that a child that is a leaf costs no frame (interp in compare/, a fifth
// faster). s is a pure read, evaluated again by the callee as c is.
#pragma once

namespace goose {

// Everything the pass needs from the optimizer — the Inliner copier, the
// inline thresholds, the node constructors, the VarDef facts and the walk
// itself — it reaches through o.
struct BaseCaseInliner {
    Optimizer &o;
    Ast &ast;

    FnSpec *basespec = nullptr;   // Spec the template below belongs to.
    Node *basecond = nullptr;     // Private copy of its base-case test, and
    Node *baseexpr = nullptr;     //   of the result (null: a valueless one).
    bool basenot = false;         // The test negated: the arms swap.
    // The guarded form: basecond is the recursing test, and the base case
    // is these statements followed by baseexpr.
    bool guarded = false;
    vector<Node *> basestmts;
    // The match form: the scrutinee and the leaf arms, to which a self-call
    // adds a wildcard arm of its own; and the self-calls it leaves as they
    // are, those an integer function returns the value of (or one side of
    // an associative operator's), which tail-recursion elimination
    // (optimize_tre.h) makes a loop of instead.
    MatchExpr *basematch = nullptr;
    set<const Call *> tailcalls;

    BaseCaseInliner(Optimizer &_o) : o(_o), ast(_o.ast) {}

    // A value of this type is fixed-size (§1.1), so binding one to a local
    // adds no storage the function has to own. Conservative wherever the size
    // class needs an instantiation the checker's per-type cache does not have
    // to hand: missing one only skips a rewrite.
    static bool FixedType(TypeExpr *t) {
        switch (t->kind) {
            case TY_INT:  return t->intstorage != IS_VARINT;
            case TY_REF:  return t->ref->lenstorage != IS_VARINT;
            case TY_FLT: case TY_BOOL: case TY_SLICE: return true;
            case TY_STRUCT: return t->struc->inst && t->struc->inst->sclass == SC_FIXED;
            case TY_ENUM: return !t->enu->varmode;
            case TY_ARRAY: return t->arr->akind == A_FIXED;
            default: return false;
        }
    }

    // The direct self-call sites the rewrite would copy the base case to, in
    // either spelling: `f(a, b)` or `a.f(b)`.
    static int SelfCalls(Node *n, FnSpec *sp) {
        if (!n) return 0;
        auto k = 0;
        if (auto c = Is<Call>(n))
            if (c->spec == sp && c->builtin < 0 && c->dispatch.empty() &&
                (Is<Ident>(c->callee) || Is<Dot>(c->callee))) k++;
        // The children include a UFCS receiver, which can be one too.
        RunChildren(n, [&](Node *ch) { k += SelfCalls(ch, sp); });
        return k;
    }

    // Is this part of the base case copyable to a call site? Free names must
    // be parameters or globals (no other local of ours is in scope there),
    // nothing may jump out of the subtree, and nothing may call back into the
    // cycle. `pure` restricts the node kinds to reads on top of that, for the
    // test, which the recursing arm evaluates a second time.
    bool BaseOK(Node *n, set<VarDef *> &bound, bool pure) {
        if (!n) return true;
        if (auto id = Is<Ident>(n)) {
            auto v = id->vdef;   // Null: a function reference.
            if (!v || v->isglobal || v->ownerspec != o.curspec) return true;
            return v->isparam || bound.count(v) > 0;
        }
        if (pure && !(Is<IntLit>(n) || Is<FltLit>(n) || Is<BoolLit>(n) || Is<StrLit>(n) ||
                      Is<NullLit>(n) || Is<Unary>(n) || Is<Binary>(n) || Is<Dot>(n) ||
                      Is<AsCast>(n) || Is<Index>(n) || Is<SliceExpr>(n)))
            return false;
        // A jump out of the subtree would land in the caller instead of the
        // callee; a nested declaration is a separate tree left behind. Every
        // return is one: Setup reads a recursive function's body before it
        // is optimized, and nothing is ever inlined into one (TryInline).
        assert(!Is<InlineBlock>(n));
        if (Is<Break>(n) || Is<Continue>(n) || Is<Return>(n) || Is<FnDecl>(n) || Is<FunVal>(n))
            return false;
        if (auto vd = Is<VarDecl>(n)) for (auto d : vd->defs) bound.insert(d);
        if (auto fl = Is<ForLoop>(n)) {
            bound.insert(fl->vdef);
            if (fl->idxdef) bound.insert(fl->idxdef);
        }
        if (auto m = Is<MatchExpr>(n)) for (auto &a : m->arms) if (a.binder) bound.insert(a.binder);
        if (auto c = Is<Call>(n)) {
            // A callee that may `return ... from` us would leave the frame
            // the copy is in, the caller's, rather than the callee's.
            auto bad = [&](FnSpec *k) {
                return k && (k->incycle || k->sf->isrec || k->needs.count(o.curspec));
            };
            if (bad(c->spec)) return false;
            for (auto d : c->dispatch) if (bad(d)) return false;
            for (auto p : c->fvparams) bound.insert(p);
        }
        auto ok = true;
        RunChildren(n, [&](Node *ch) { ok = ok && BaseOK(ch, bound, pure); });
        return ok;
    }

    // Records a private copy of sp's base case when it has the shape above.
    // Called with o.curspec == sp before its body is optimized, so the copy
    // taken here is not disturbed by the rewriting that then happens in place.
    void Setup(FnSpec *sp) {
        basespec = nullptr;
        basecond = baseexpr = nullptr;
        basenot = guarded = false;
        basestmts.clear();
        basematch = nullptr;
        tailcalls.clear();
        if (!o.caninline || !sp->sf->isrec || !sp->body) return;
        // Binding a non-fixed parameter to a temporary would give a cycle
        // member a non-fixed local in scope at the self-call, which §7.8
        // forbids — and copy the value at every level besides, where
        // passing it costs nothing.
        for (auto pv : sp->params) if (!FixedType(pv->type)) return;
        if (!SetupLeading(sp) && !SetupMatch(sp)) SetupGuarded(sp);
    }

    // The match form: the body is `match s { ... }`, as its value or as
    // `return match s { ... };`, over an enum's variants with no wildcard,
    // some arms recursing and some not.
    bool SetupMatch(FnSpec *sp) {
        auto body = sp->body;
        Node *only = body->stmts.empty() ? body->tail : body->stmts.size() == 1 && !body->tail ? body->stmts[0]
                                                                                              : nullptr;
        if (auto r = only ? Is<Return>(only) : nullptr) {
            if (r->target != sp->sf || !r->from.empty() || r->vals.size() != 1 || sp->rets.size() != 1)
                return false;
            only = r->vals[0];
        } else if (only != body->tail || sp->rets.size() != 1) {
            return false;
        }
        auto m = only ? Is<MatchExpr>(only) : nullptr;
        if (!m) return false;
        set<VarDef *> bound;
        if (!BaseOK(m->scrutinee, bound, true)) return false;
        vector<const MatchArm *> leaves;
        auto recursing = 0;
        auto sz = CountNodes(m->scrutinee);
        for (auto &arm : m->arms) {
            if (arm.pat.kind == P_WILDCARD || arm.variants.empty() || !arm.ranges.empty()) return false;
            if (SelfCalls(arm.body, sp)) {
                recursing++;
                continue;
            }
            set<VarDef *> armbound = bound;
            if (arm.binder) armbound.insert(arm.binder);
            if (!BaseOK(arm.body, armbound, false)) return false;
            sz += CountNodes(arm.body);
            leaves.push_back(&arm);
        }
        if (leaves.empty() || !recursing || !SizeOK(sp, sz)) return false;
        Inliner inl { o, ast, sp, sp, {}, {} };
        for (auto pv : sp->params) inl.vmap[pv] = pv;   // Parameters stay themselves.
        basematch = ast.New<MatchExpr>(m->line, inl.Cp(m->scrutinee));
        for (auto arm : leaves) basematch->arms.push_back(CpArm(inl, *arm));
        basematch->exprtype = m->exprtype;
        basespec = sp;
        if (sp->rets[0]->kind == TY_INT) {
            TailCalls(body->tail, sp);
            ReturnedCalls(body, sp);
        }
        return true;
    }

    // The self-calls of a result: the value itself, either side of an
    // associative operator, and the values of the branches it is made of.
    void TailCalls(Node *n, FnSpec *sp) {
        if (!n) return;
        if (auto c = Is<Call>(n)) {
            if (c->spec == sp) tailcalls.insert(c);
        } else if (auto b = Is<Binary>(n)) {
            if (b->op == T_PLUS || b->op == T_MUL || b->op == T_BITAND || b->op == T_BITOR || b->op == T_XOR) {
                TailCalls(b->left, sp);
                TailCalls(b->right, sp);
            }
        } else if (auto bl = Is<Block>(n)) {
            TailCalls(bl->tail, sp);
        } else if (auto x = Is<IfExpr>(n)) {
            TailCalls(x->thenb, sp);
            TailCalls(x->elseb, sp);
        } else if (auto m = Is<MatchExpr>(n)) {
            for (auto &arm : m->arms) TailCalls(arm.body, sp);
        }
    }
    // The same for every value the function returns.
    void ReturnedCalls(Node *n, FnSpec *sp) {
        if (!n || Is<FunVal>(n) || Is<FnDecl>(n)) return;
        if (auto r = Is<Return>(n); r && r->target == sp->sf && r->vals.size() == 1) TailCalls(r->vals[0], sp);
        RunChildren(n, [&](Node *ch) { ReturnedCalls(ch, sp); });
    }

    // An enum arm copied as MatchExpr::Cp1 copies it.
    static MatchArm CpArm(Inliner &inl, const MatchArm &arm) {
        MatchArm a;
        a.pat = arm.pat;
        a.variants = arm.variants;
        a.binder = inl.Remap(arm.binder);
        a.body = inl.Cp(arm.body);
        return a;
    }

    // The inliner's size rule, against what the rewrite duplicates: sz
    // nodes, once per self-call site.
    bool SizeOK(FnSpec *sp, int sz) {
        auto sites = SelfCalls(sp->body, sp);
        return sites && (sz < o.nc || sz * sites < o.ncu);
    }

    // The base case the body starts with.
    bool SetupLeading(FnSpec *sp) {
        auto &stmts = sp->body->stmts;
        auto ife = Is<IfExpr>(stmts.empty() ? sp->body->tail : stmts[0]);
        if (!ife) return false;
        auto cond = ife->cond;
        auto neg = ife->elseb != nullptr;
        auto arm = neg ? Is<Block>(ife->elseb) : ife->thenb;
        if (!arm || arm->tail || arm->stmts.size() != 1) return false;
        auto r = Is<Return>(arm->stmts[0]);
        if (!r || r->target != sp->sf || !r->from.empty()) return false;
        if (r->vals.size() > 1 || r->vals.size() != sp->rets.size()) return false;
        auto expr = r->vals.empty() ? nullptr : r->vals[0];
        set<VarDef *> bound;
        if (!BaseOK(cond, bound, true)) return false;
        if (!BaseOK(expr, bound, false)) return false;
        if (!SizeOK(sp, CountNodes(cond) + CountNodes(expr))) return false;
        Inliner inl { o, ast, sp, sp, {}, {} };
        for (auto pv : sp->params) inl.vmap[pv] = pv;   // Parameters stay themselves.
        basecond = inl.Cp(cond);
        if (expr) baseexpr = inl.Cp(expr);
        basenot = neg;
        basespec = sp;
        return true;
    }

    // A test the call site can evaluate before the body's first statements
    // run and get the callee's answer: comparisons of literals and of
    // primitive scalar parameters the body never assigns, which cannot abort.
    bool GuardOK(Node *n, FnSpec *sp) {
        if (Is<IntLit>(n) || Is<FltLit>(n) || Is<BoolLit>(n) || Is<NullLit>(n)) return true;
        if (auto id = Is<Ident>(n)) {
            auto v = id->vdef;
            if (!v || !v->isparam || v->ownerspec != sp) return false;
            // Write facts describe the binding. Composite and reference-like
            // values can still observe storage changed through another binding.
            return Optimizer::Unwritten(o.facts, v);
        }
        if (auto u = Is<Unary>(n)) return u->op == T_NOT && GuardOK(u->child, sp);
        if (auto b = Is<Binary>(n)) {
            switch (b->op) {
                case T_EQ: case T_NEQ: case T_LT: case T_GT: case T_LTEQ: case T_GTEQ:
                case T_ANDAND: case T_OROR:
                    return GuardOK(b->left, sp) && GuardOK(b->right, sp);
                default: return false;
            }
        }
        return false;
    }

    // The guarded form: `s1...; if c { ... } s3...; return e;`, with every
    // self-call inside the if.
    void SetupGuarded(FnSpec *sp) {
        auto body = sp->body;
        auto sites = SelfCalls(body, sp);
        if (!sites) return;
        vector<Node *> all = body->stmts;
        if (body->tail) all.push_back(body->tail);
        size_t k = 0;
        IfExpr *ife = nullptr;
        for (; k < all.size(); k++) {
            ife = Is<IfExpr>(all[k]);
            if (ife && SelfCalls(ife->thenb, sp) == sites) break;
            if (SelfCalls(all[k], sp)) return;
        }
        if (k == all.size() || ife->elseb || !GuardOK(ife->cond, sp)) return;
        // What follows the if: statements, then the result as a final
        // `return e` or as the body's tail, or none at all.
        vector<Node *> pre(all.begin(), all.begin() + k), post(all.begin() + k + 1, all.end());
        Node *expr = nullptr;
        if (!post.empty() && post.back() == body->tail) {
            if (sp->rets.size() != 1) return;
            expr = post.back();
            post.pop_back();
        } else if (auto r = post.empty() ? nullptr : Is<Return>(post.back())) {
            if (r->target != sp->sf || !r->from.empty() || r->vals.size() > 1 ||
                r->vals.size() != sp->rets.size())
                return;
            expr = r->vals.empty() ? nullptr : r->vals[0];
            post.pop_back();
        } else if (!sp->rets.empty()) {
            return;
        }
        set<VarDef *> bound;
        auto sz = CountNodes(ife->cond) + CountNodes(expr);
        for (auto part : { &pre, &post })
            for (auto st : *part) {
                if (!BaseOK(st, bound, false)) return;
                sz += CountNodes(st);
            }
        if (!BaseOK(expr, bound, false) || !SizeOK(sp, sz)) return;
        Inliner inl { o, ast, sp, sp, {}, {} };
        for (auto pv : sp->params) inl.vmap[pv] = pv;   // Parameters stay themselves.
        basecond = inl.Cp(ife->cond);
        for (auto part : { &pre, &post })
            for (auto st : *part) basestmts.push_back(inl.Cp(st));
        if (expr) baseexpr = inl.Cp(expr);
        guarded = true;
        basespec = sp;
    }

    // Rewrites one self-call against the recorded base case.
    Node *Try(Call *c) {
        // The template is only good inside the body it was set up for: the
        // global initializers the driver optimizes after its loop are not it.
        if (!basespec || basespec != o.curspec) return nullptr;
        if (c->spec != basespec || c->builtin >= 0 || !c->dispatch.empty()) return nullptr;
        if (!Is<Ident>(c->callee) && !Is<Dot>(c->callee)) return nullptr;
        if (basematch && tailcalls.count(c)) return nullptr;
        auto K = basespec;
        // The arguments in parameter order, a UFCS receiver first: the checker
        // has already made each the node its parameter takes (an `&` it binds
        // by reference included), so a receiver binds as any argument does.
        auto argnodes = c->ValueArgNodes();
        if (argnodes.size() != K->params.size()) return nullptr;
        // Passed where a slice is expected, the call was checked as that
        // slice (§3.10), while the arms would yield the array itself: built
        // inside an arm, it would be released before the slice is used.
        if (c->exprtype && c->exprtype->kind == TY_SLICE && K->rets.size() == 1 &&
            K->rets[0]->kind == TY_ARRAY)
            return nullptr;
        Inliner inl { o, ast, K, o.curspec, {}, {} };
        vector<Node *> decls;
        for (size_t i = 0; i < K->params.size(); i++) {
            auto pv = K->params[i];
            auto vd = inl.BindArg(pv, argnodes[i], c->line);
            if (!vd) continue;
            vd->inline_arg = true;
            decls.push_back(vd);
            // The recursing arm passes the binding on, so whichever arm runs,
            // the argument was evaluated exactly once and before the test.
            auto id = ast.New<Ident>(c->line, pv->name);
            id->vdef = vd->defs[0];
            id->exprtype = pv->type;
            c->SetArgNode(i, id);
        }
        auto vt = c->exprtype ? c->exprtype : ast.voidtype;
        if (basematch) {
            // The leaf arms against the bindings, then the call for the rest.
            auto m = (MatchExpr *)inl.Cp(basematch);
            auto around = decls.empty() ? 1 : 2;
            o.depth += around;
            m->scrutinee = o.Opt(m->scrutinee);
            o.branchdepth++;
            for (auto &arm : m->arms) arm.body = o.OptIn(m, arm.body);
            o.branchdepth--;
            o.depth -= around;
            MatchArm rest;
            rest.pat.kind = P_WILDCARD;
            rest.body = c;
            m->arms.push_back(rest);
            m->exprtype = vt;
            o.basecases++;
            if (decls.empty()) return m;
            auto blk = ast.New<Block>(c->line);
            blk->stmts = std::move(decls);
            blk->tail = m;
            blk->exprtype = vt;
            return blk;
        }
        auto cond = o.Opt(inl.Cp(basecond));
        auto arm = [&](Node *v) {
            auto b = ast.New<Block>(c->line);
            if (v) b->tail = v;
            b->exprtype = vt;
            return b;
        };
        IfExpr *ife;
        if (guarded) {
            // The base case is the else arm, a block inside the one holding
            // the bindings if there are any.
            auto leaf = arm(nullptr);
            for (auto st : basestmts) leaf->stmts.push_back(inl.Cp(st));
            if (baseexpr) leaf->tail = inl.Cp(baseexpr);
            auto around = decls.empty() ? 0 : 1;
            o.depth += around;
            o.OptBlock(leaf);
            o.depth -= around;
            ife = ast.New<IfExpr>(c->line, cond, arm(c), leaf);
        } else {
            // The result lands in an arm of the `if` built below, inside the
            // block holding the bindings if there are any.
            auto around = decls.empty() ? 1 : 2;
            o.depth += around;
            auto val = baseexpr ? o.Opt(inl.Cp(baseexpr)) : nullptr;
            o.depth -= around;
            // A negated test is the continuing case, so its arms are the
            // other way round.
            ife = ast.New<IfExpr>(c->line, cond, arm(basenot ? (Node *)c : val),
                                  arm(basenot ? val : (Node *)c));
        }
        ife->exprtype = vt;
        o.basecases++;
        if (decls.empty()) return ife;
        auto blk = ast.New<Block>(c->line);
        blk->stmts = std::move(decls);
        blk->tail = ife;
        blk->exprtype = vt;
        return blk;
    }
};

// The optimizer's two hooks into the pass. The object is made on first use:
// it needs the Inliner, so it cannot be a member of the Optimizer itself.
inline void Optimizer::SetupBaseCase(FnSpec *sp) {
    if (!basecase) basecase = make_unique<BaseCaseInliner>(*this);
    basecase->Setup(sp);
}

inline Node *Optimizer::TryBaseCase(Call *c) {
    return basecase ? basecase->Try(c) : nullptr;
}

}  // namespace goose
