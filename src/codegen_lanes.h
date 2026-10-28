// Gust: outer-loop lane vectorization (ROADMAP Phase 5).
//
// A loop over an array's elements whose iterations are independent runs
// four of them at once, one per lane of a vector of doubles: n-body's force
// loop, whose iteration is one body's acceleration from every other. Each
// lane computes exactly what its iteration computes, operation by
// operation, so every result is the same to the bit (a float operation
// rounds on its own, runtime.h); only which lanes' operations share an
// instruction changes.
//
//     for bi, i in bodies {             for (; n - i >= 4; i += 4) {
//         let pi = bi.p;                    bi_l: each field a gs_l4d of bodies[i..i+3]'s
//         var a = double3_0;                pi_l = bi_l.p; a_l = splat(double3_0)
//         for bj in bodies {                for (j...) { bj = bodies[j]; ...each lane... }
//             ...                           acc[i + l] = a_l's lane l, for l < 4
//         }                             }
//         acc[i] = a;                   for (; i < n; i++) { the loop as it is }
//     }
//
// The lanes are C vector types (`vector_size`, runtime_ext.h's GS_LANES):
// the C compiler's own vectorizer had made the same loop twice as slow
// (ROADMAP Phase 5), while explicit lanes leave it the instructions alone to
// pick. clang alone compiles the block of lanes: MSVC, TinyCC and MIR have
// no vector types, and gcc's code for them is slower than the loop as it
// is. Elsewhere the loop runs as it is.
//
// What a body may hold, checked before anything is emitted (LaneGen::Plan):
// locals of doubles and of structs of them, arithmetic, field reads, sqrt,
// loads of any array at an index every lane shares, loads and stores at
// the loop's own index, and inner loops over a range or another array,
// whose iteration every lane shares. A value is a lane value (one per lane)
// where it derives from the loop's element or a load at its index, and
// is uniform otherwise: emitted once, as the scalar code would, and spread
// across the lanes where a lane value meets it. Nothing in the body may
// write anything but its own locals and the arrays it stores to at the
// loop's index, which nothing else in it reads: so the iterations are
// independent, and an abort, which only a uniform operation can raise
// there, comes in the lane the loop's first iteration of the block would
// have raised it in, before any of the block's stores.
//
// An `if` whose condition every lane shares is an `if` as it is. One whose
// condition differs by lane (comparisons of doubles, joined by &&, || and
// !) runs both branches in every lane, each assignment storing into the
// lanes its branch is taken in (a mask, gs_l4m, blended bit by bit), and an
// `if` with a value takes each lane's from its own branch. So both branches
// run where one iteration at a time runs one: they may store into no array
// and run no loop, and nothing in them may abort (no checked index, no
// integer arithmetic, no conversion but to a float).
//
// The filter form (PlanFilter), for a search: a body of pure statements
// ending in `if c1 && c2 ... { anything }`, as a ray tracer's loop over
// its spheres is. The lanes compute the statements and the conditions
// they can for four iterations at once; where no lane's hold, none of the
// four would have done anything, and they are skipped, and otherwise the
// four run as the loop is. That is exact whatever the if does: until the
// first iteration whose conditions hold, nothing has changed, so the lanes
// see that one hold. Elements may be bound by reference, and an inlined
// function may return early: each return takes its value into the lanes
// still running, which then stop (RetBlock, RetEmit).
#pragma once

namespace goose {

struct LaneGen {
    CodeGen &cg;
    ForLoop *f;
    static constexpr int K = 4;
    VarDef *idx = nullptr;      // The loop's index binder, used only to index at the lane.
    VarDef *elem = nullptr;     // Its element, bound by value.
    unordered_set<const VarDef *> lanev;     // Locals holding a lane value.
    unordered_set<const VarDef *> inner;     // Locals the body declares.
    unordered_set<const VarDef *> stored;    // Arrays stored to at the loop's index.
    unordered_map<const VarDef *, int> atidx, elsewhere;   // Each array's uses.
    vector<const VarDef *> indexed;          // Those used at the index, as first met.
    vector<TypeExpr *> structs;              // The lane struct types, inner ones first.
    bool innerloop = false, lanefloat = false;
    // The filter form: the pure statements, the if they end in, and the
    // conditions of its && the lanes test.
    bool filtering = false;
    vector<Node *> prefix;
    IfExpr *gate = nullptr;
    vector<Node *> keys;
    // References to the element: the loop's binder of `for &s`, and the
    // locals an inlined call binds to it. Each is the element's lanes.
    unordered_set<const VarDef *> refs;

    // The element's type, through a reference.
    TypeExpr *ElemT() { return elem->type->kind == TY_REF ? elem->type->ref->sub : elem->type; }
    // A lane value's type, a reference to the element being the element's.
    TypeExpr *LaneT(Node *n) {
        if (auto id = Is<Ident>(n); id && refs.count(id->vdef)) return ElemT();
        return n->exprtype;
    }

    LaneGen(CodeGen &c, ForLoop *l) : cg(c), f(l) {}

    // ------------------------------------------------------------------
    // The plan.

    // A double, or a struct of them at any depth: a value of which each lane
    // holds its own.
    bool Laneable(TypeExpr *t, int depth = 0) {
        if (!t || depth > 8) return false;
        if (t->kind == TY_FLT) return t->fltstorage == FS_F64;
        if (t->kind != TY_STRUCT) return false;
        auto si = cg.SI(t);
        if (si->st->fields.empty() || si->sclass != SC_FIXED) return false;
        for (size_t i = 0; i < si->st->fields.size(); i++)
            if (si->st->fields[i].ispad || !Laneable(si->ftypes[i], depth + 1)) return false;
        return true;
    }

    void NoteStruct(TypeExpr *t) {
        if (t->kind != TY_STRUCT) return;
        auto si = cg.SI(t);
        for (auto ft : si->ftypes) NoteStruct(ft);
        for (auto s : structs)
            if (cg.SI(s) == si) return;
        structs.push_back(t);
    }

    static bool IsVar(Node *n, const VarDef *v) {
        auto id = Is<Ident>(n);
        return v && id && id->vdef == v;
    }

    static const VarDef *RootLocal(Node *n) {
        if (auto id = Is<Ident>(n)) return id->vdef;
        if (auto d = Is<Dot>(n); d && d->IsField()) return RootLocal(d->obj);
        return nullptr;
    }

    // math's sqrt, the one call a body may make.
    static bool IsSqrt(Call *c) {
        auto sf = c->spec ? c->spec->sf : nullptr;
        return sf && sf->isextern && sf->cname == "gs_sqrt" && c->args.size() == 1 && !Is<Dot>(c->callee);
    }

    // The element type of an array, a slice, or a reference to either.
    static TypeExpr *ElemOf(TypeExpr *t) {
        if (t->kind == TY_REF) t = t->ref->sub;
        if (t->kind == TY_ARRAY) return t->arr->sub;
        if (t->kind == TY_SLICE) return t->sub;
        return nullptr;
    }

    // The array an index reads, where it is a variable's.
    static const VarDef *ArrayVar(Index *x) {
        auto id = Is<Ident>(x->obj);
        return id ? id->vdef : nullptr;
    }

    bool Lane(Node *n) {
        if (!n) return false;
        switch (n->nkind) {
            case NK_Ident: return lanev.count(static_cast<Ident *>(n)->vdef) > 0;
            case NK_Dot: return Lane(static_cast<Dot *>(n)->obj);
            case NK_Binary: {
                auto b = static_cast<Binary *>(n);
                return Lane(b->left) || Lane(b->right);
            }
            case NK_Unary: return Lane(static_cast<Unary *>(n)->child);
            case NK_AsCast: return Lane(static_cast<AsCast *>(n)->child);
            case NK_Call: {
                for (auto a : static_cast<Call *>(n)->args)
                    if (Lane(a)) return true;
                return false;
            }
            case NK_Index: return IsVar(static_cast<Index *>(n)->idx, idx);
            case NK_InlineBlock: {
                // One that returns early differs by lane where anything in it does.
                auto ib = static_cast<InlineBlock *>(n);
                return Lane(ib->body->tail) || (HasReturn(ib->body, ib->sf) && LaneAny(ib->body));
            }
            case NK_StructLit: {
                for (auto &fi : static_cast<StructLit *>(n)->inits)
                    if (Lane(fi.val)) return true;
                return false;
            }
            case NK_IfExpr: {
                auto x = static_cast<IfExpr *>(n);
                return Lane(x->cond) || Lane(x->thenb->tail) || Lane(BranchTail(x->elseb));
            }
            case NK_Block: return Lane(static_cast<goose::Block *>(n)->tail);
            default: return false;
        }
    }

    // Whether anything in n reads a lane value.
    bool LaneAny(Node *n) {
        if (!n) return false;
        if (auto id = Is<Ident>(n)) return lanev.count(id->vdef) > 0;
        if (auto x = Is<Index>(n); x && IsVar(x->idx, idx)) return true;
        auto found = false;
        n->Children([&](Node *c) { found = found || LaneAny(c); });
        return found;
    }

    // Whether n holds a return from sf, an inlined function.
    static bool HasReturn(Node *n, SFunction *sf) {
        if (!n || Is<FunVal>(n) || Is<FnDecl>(n)) return false;
        if (auto r = Is<Return>(n)) return r->target == sf;
        auto found = false;
        n->Children([&](Node *c) { found = found || HasReturn(c, sf); });
        return found;
    }

    // A branch's value: its block's tail, or the `else if` it is.
    static Node *BranchTail(Node *b) {
        if (auto bl = Is<goose::Block>(b)) return bl->tail;
        return b;
    }

    // Marks the locals a lane value reaches, through declarations and
    // assignments, until none is left to mark. A local assigned under a
    // condition that differs by lane holds a lane value too: each lane
    // keeps its own branch's.
    void Classify() {
        if (elem) lanev.insert(elem);
        for (auto changed = true; changed;) {
            changed = false;
            function<void(Node *, bool)> walk = [&](Node *n, bool under) {
                if (!n) return;
                if (auto x = Is<IfExpr>(n)) {
                    walk(x->cond, under);
                    auto m = under || Lane(x->cond);
                    walk(x->thenb, m);
                    walk(x->elseb, m);
                    return;
                }
                if (auto vd = Is<VarDecl>(n)) {
                    for (size_t i = 0; i < vd->defs.size() && i < vd->inits.size(); i++)
                        if (!lanev.count(vd->defs[i]) && Lane(vd->inits[i]))
                            changed = lanev.insert(vd->defs[i]).second || changed;
                } else if (auto a = Is<Assign>(n)) {
                    if (auto v = RootLocal(a->lval); v && !lanev.count(v) && (under || Lane(a->rhs)))
                        changed = lanev.insert(v).second || changed;
                }
                n->Children([&](Node *c) { walk(c, under); });
            };
            // The filter form's if runs one iteration at a time: only what
            // comes before it and its conditions are the lanes'.
            if (filtering) {
                for (auto s : prefix) walk(s, false);
                walk(gate->cond, false);
            } else {
                walk(f->body, false);
            }
        }
    }

    bool NoJumps(Node *n) {
        if (!n) return true;
        if (Is<Return>(n) || Is<Break>(n) || Is<Continue>(n) || Is<FunVal>(n) || Is<FnDecl>(n)) return false;
        auto ok = true;
        n->Children([&](Node *c) { ok = ok && NoJumps(c); });
        return ok;
    }

    // A fixed value a lane-local temporary can hold, for a uniform local.
    bool Plain(TypeExpr *t) { return t && t->kind != TY_REF && cg.IsFix(t) && !cg.IsLargeFixed(t); }

    // Inside a branch of a condition that differs by lane (and in such a
    // condition), where every lane runs both branches: what is there runs
    // where one iteration at a time would not have run it, so it may not
    // abort -- no checked index, no integer arithmetic, which a checked
    // build checks, no conversion but to a float -- nor store anything.
    int masked = 0;

    static bool Compare(TType op) {
        return op == T_LT || op == T_GT || op == T_LTEQ || op == T_GTEQ || op == T_EQ || op == T_NEQ;
    }

    // A condition every lane takes its own way: comparisons of doubles,
    // joined by &&, || and !, with uniform conditions among them.
    bool Cond(Node *c) {
        if (!Lane(c)) return Value(c);
        if (auto u = Is<Unary>(c); u && u->op == T_NOT) return Cond(u->child);
        auto b = Is<Binary>(c);
        if (!b) return false;
        if (b->op == T_ANDAND || b->op == T_OROR) return Cond(b->left) && Cond(b->right);
        if (!Compare(b->op)) return false;
        for (auto o : { b->left, b->right }) {
            auto ot = cg.OperandT(o->exprtype);
            if (!ot || ot->kind != TY_FLT || ot->fltstorage != FS_F64) return false;
        }
        lanefloat = true;
        return Value(b->left) && Value(b->right);
    }

    // A branch of an `if`, a block or an `else if`, whose value is wanted
    // or not.
    bool Branch(Node *b, bool value) {
        if (!b) return !value;
        if (auto x = Is<IfExpr>(b)) return value ? Value(x) : Stmt(x);
        // The optimizer leaves a branch of one expression bare.
        auto bl = Is<goose::Block>(b);
        if (!bl) return value ? Value(b) : Stmt(b);
        if (!value) return Body(bl, true);
        return bl->tail && Body(bl, false) && Value(bl->tail);
    }

    bool IfParts(IfExpr *x, bool value) {
        if (!Lane(x->cond)) return Value(x->cond) && Branch(x->thenb, value) && Branch(x->elseb, value);
        masked++;
        auto ok = Cond(x->cond) && Branch(x->thenb, value) && Branch(x->elseb, value);
        masked--;
        return ok;
    }

    bool Value(Node *n) {
        if (!n) return false;
        auto lane = Lane(n);
        if (lane && n->exprtype) {
            if (!Laneable(LaneT(n))) return false;
            NoteStruct(LaneT(n));
        }
        switch (n->nkind) {
            case NK_IntLit: case NK_FltLit: case NK_BoolLit: return true;
            case NK_Ident: {
                auto v = static_cast<Ident *>(n)->vdef;
                if (!v || v == idx) return false;
                if (stored.count(v)) elsewhere[v]++;
                return true;
            }
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                if (lane && !d->IsField()) return false;
                return Value(d->obj);
            }
            case NK_Binary: {
                auto b = static_cast<Binary *>(n);
                if (masked && cg.OperandT(b->left->exprtype)->kind == TY_INT && !Compare(b->op)) return false;
                if (lane) {
                    if (b->op != T_PLUS && b->op != T_MINUS && b->op != T_MUL && b->op != T_DIV) return false;
                    lanefloat = true;
                    // A scalar operand of an elementwise operation is a double.
                    for (auto o : { b->left, b->right }) {
                        auto ot = cg.OperandT(o->exprtype);
                        if (!Laneable(ot)) return false;
                        if (ot->kind != b->exprtype->kind && ot->kind != TY_FLT) return false;
                    }
                }
                return Value(b->left) && Value(b->right);
            }
            case NK_Unary: {
                auto u = static_cast<Unary *>(n);
                if (lane && u->op != T_MINUS) return false;
                if (!lane && u->op == T_BITAND) return false;
                return Value(u->child);
            }
            case NK_AsCast: {
                auto c = static_cast<AsCast *>(n);
                if (masked && (!c->exprtype || c->exprtype->kind != TY_FLT)) return false;
                if (lane && !(c->child->exprtype && c->child->exprtype->kind == TY_FLT &&
                              c->child->exprtype->fltstorage == FS_F64))
                    return false;
                return Value(c->child);
            }
            case NK_Call: {
                auto c = static_cast<Call *>(n);
                if (!IsSqrt(c)) return false;
                lanefloat = lanefloat || lane;
                return Value(c->args[0]);
            }
            case NK_Index: {
                auto x = static_cast<Index *>(n);
                auto v = ArrayVar(x);
                if (!v) return false;
                if (IsVar(x->idx, idx)) {
                    if (!atidx[v]++ && !stored.count(v)) indexed.push_back(v);
                    return true;
                }
                if (Lane(x->idx) || (masked && !x->nobc)) return false;
                elsewhere[v]++;
                return Value(x->idx);
            }
            case NK_InlineBlock: {
                auto ib = static_cast<InlineBlock *>(n);
                if (NoJumps(ib->body)) return ib->body->tail && Body(ib->body, false) && Value(ib->body->tail);
                // Returning early, in the filter form only.
                auto ends = false;
                return filtering && RetBlock(ib->body, ib->sf, ends) && ends;
            }
            case NK_StructLit: {
                auto sl = static_cast<StructLit *>(n);
                if (!sl->sinst || sl->einst) return false;
                if (lane) {
                    auto &fs = sl->sinst->st->fields;
                    for (size_t i = 0; i < fs.size(); i++)
                        if (!sl->InitFor((int)i)) return false;
                }
                for (auto &fi : sl->inits)
                    if (!Value(fi.val)) return false;
                return true;
            }
            case NK_IfExpr: return IfParts(static_cast<IfExpr *>(n), true);
            default: return false;
        }
    }

    bool Stmt(Node *s) {
        if (!s) return true;
        switch (s->nkind) {
            case NK_VarDecl: {
                auto vd = static_cast<VarDecl *>(s);
                if (vd->defs.size() != 1 || vd->inits.size() != 1 || vd->reusable) return false;
                auto v = vd->defs[0];
                // An inlined call's parameter bound to the element by reference.
                if (auto id = Is<Ident>(vd->inits[0]); id && refs.count(id->vdef) && v->type &&
                                                        v->type->kind == TY_REF && !v->isglobal && !v->captured) {
                    refs.insert(v);
                    lanev.insert(v);
                    inner.insert(v);
                    return true;
                }
                if (vd->byref) return false;
                if (v->isglobal || v->captured || v->reusable || !Plain(v->type)) return false;
                inner.insert(v);
                if (lanev.count(v)) {
                    if (!Laneable(v->type)) return false;
                    NoteStruct(v->type);
                }
                return Value(vd->inits[0]);
            }
            case NK_Assign: {
                auto a = static_cast<Assign *>(s);
                auto compound = a->op == T_PLUSEQ || a->op == T_MINUSEQ || a->op == T_MULEQ || a->op == T_DIVEQ;
                if (a->op != T_ASSIGN && !compound) return false;
                if (auto x = Is<Index>(a->lval); x && IsVar(x->idx, idx)) {
                    if (masked) return false;
                    auto v = ArrayVar(x);
                    // Not compound, and not a variable a nested function
                    // writes too; what else may reach it is Plan's to rule out.
                    if (!v || v->captured || !v->type || !ElemOf(v->type) || compound ||
                        !Laneable(x->exprtype))
                        return false;
                    NoteStruct(x->exprtype);
                    if (stored.insert(v).second && !atidx.count(v)) indexed.push_back(v);
                    return Value(a->rhs);
                }
                auto root = RootLocal(a->lval);
                if (!root || !inner.count(root)) return false;
                if (compound && (!a->lval->exprtype || a->lval->exprtype->kind != TY_FLT)) return false;
                if (lanev.count(root)) return Value(a->rhs) && (!Is<Dot>(a->lval) || Value(a->lval));
                if (compound) return false;
                return Value(a->lval) && Value(a->rhs);
            }
            case NK_ForLoop: {
                auto fl = static_cast<ForLoop *>(s);
                if (masked || fl->parallel || !fl->label.empty() || fl->byref) return false;
                if (fl->vdef && (lanev.count(fl->vdef) || !Plain(fl->vdef->type))) return false;
                if (fl->idxdef && lanev.count(fl->idxdef)) return false;
                if (Lane(fl->iter)) return false;
                if (auto r = Is<RangeExpr>(fl->iter)) {
                    if (!Value(r->lo) || !Value(r->hi)) return false;
                } else if (auto id = Is<Ident>(fl->iter); id && id->vdef && id->vdef->type &&
                                                          (id->vdef->type->kind == TY_ARRAY ||
                                                           IsRefOrSlice(id->vdef->type))) {
                    if (!fl->vdef || !fl->vdef->copybind) return false;
                    elsewhere[id->vdef]++;
                } else if (!fl->iter->exprtype || fl->iter->exprtype->kind != TY_INT || !Value(fl->iter)) {
                    return false;
                }
                if (fl->vdef) inner.insert(fl->vdef);
                if (fl->idxdef) inner.insert(fl->idxdef);
                innerloop = true;
                return Body(fl->body, true);
            }
            case NK_InlineBlock: {
                auto ib = static_cast<InlineBlock *>(s);
                return NoJumps(ib->body) && Body(ib->body, true);
            }
            case NK_Block: return Body(static_cast<Block *>(s), true);
            case NK_IfExpr: return IfParts(static_cast<IfExpr *>(s), false);
            default: return false;
        }
    }

    // A block's statements, and its tail where the block is a statement.
    bool Body(Block *b, bool tailstmt) {
        for (auto s : b->stmts)
            if (!Stmt(s)) return false;
        return !tailstmt || !b->tail || Stmt(b->tail);
    }

    // An inlined body of sf that returns early, each return with one value:
    // as a return of its own, or ending an if's branch. Both branches of
    // such an if run in every lane, as does all that follows one some lanes
    // return in, so none of that may abort (masked). `ends`: every lane has
    // its value at the end.
    bool RetBlock(Block *b, SFunction *sf, bool &ends) {
        ends = false;
        vector<Node *> all = b->stmts;
        if (b->tail) all.push_back(b->tail);
        auto added = 0;
        auto ok = true;
        for (size_t i = 0; ok && i < all.size(); i++) {
            auto s = all[i];
            auto last = i + 1 == all.size();
            if (Is<Return>(s) || (Is<IfExpr>(s) && HasReturn(s, sf))) {
                auto e = false;
                ok = RetBranch(s, sf, e);
                if (e) {
                    ends = true;
                    ok = ok && last;
                } else {
                    masked++;
                    added++;
                }
            } else if (last && s == b->tail) {
                ok = Value(s);
                ends = true;
            } else {
                ok = Stmt(s);
            }
        }
        masked -= added;
        return ok;
    }
    bool RetBranch(Node *br, SFunction *sf, bool &ends) {
        ends = false;
        if (!br) return true;
        if (auto bl = Is<goose::Block>(br)) return RetBlock(bl, sf, ends);
        if (auto r = Is<Return>(br)) {
            ends = true;
            return r->target == sf && r->from.empty() && r->vals.size() == 1 && Value(r->vals[0]);
        }
        auto x = Is<IfExpr>(br);
        if (!x || !HasReturn(x, sf)) return Stmt(br);
        auto te = false, ee = false;
        masked++;
        auto ok = Cond(x->cond) && RetBranch(x->thenb, sf, te) && RetBranch(x->elseb, sf, ee);
        masked--;
        ends = te && ee;
        return ok;
    }

    // The filter form: what the loop's body is (the header's last part).
    bool PlanFilter() {
        if (f->parallel || !f->label.empty() || !f->vdef) return false;
        auto it = Is<Ident>(f->iter);
        if (!it || !it->vdef || !it->vdef->type) return false;
        elem = f->vdef;
        idx = f->idxdef;
        if (f->byref ? elem->type->kind != TY_REF : !elem->copybind) return false;
        if (!Laneable(ElemT()) || elem->captured || (idx && idx->captured)) return false;
        if (f->byref) refs.insert(elem);
        NoteStruct(ElemT());
        vector<Node *> all = f->body->stmts;
        if (f->body->tail) all.push_back(f->body->tail);
        if (all.empty()) return false;
        gate = Is<IfExpr>(all.back());
        if (!gate || gate->elseb) return false;
        prefix.assign(all.begin(), all.end() - 1);
        filtering = true;
        Classify();
        for (auto s : prefix)
            if (!Stmt(s)) return false;
        // Pure statements: nothing stored, so nothing the lanes skip.
        if (!stored.empty() || innerloop) return false;
        // The conditions of the if's && the lanes can test, anywhere along
        // it: each one false is the if not taken. They run in every lane,
        // where the && would have stopped at an earlier one: masked.
        vector<Node *> conj;
        function<void(Node *)> split = [&](Node *c) {
            if (auto b = Is<Binary>(c); b && b->op == T_ANDAND) {
                split(b->left);
                split(b->right);
            } else {
                conj.push_back(c);
            }
        };
        split(gate->cond);
        auto lanekey = false;
        for (auto c : conj) {
            masked++;
            auto ok = Cond(c);
            masked--;
            if (!ok) continue;
            keys.push_back(c);
            lanekey = lanekey || Lane(c);
        }
        return lanekey && lanefloat;
    }

    // Whether the loop runs in lanes, and what each part of it is.
    bool Plan() {
        if (f->parallel || !f->label.empty() || f->byref || !f->vdef || !f->vdef->copybind) return false;
        auto it = Is<Ident>(f->iter);
        if (!it || !it->vdef || !it->vdef->type) return false;
        elem = f->vdef;
        idx = f->idxdef;
        if (!Laneable(elem->type) || elem->captured || (idx && idx->captured)) return false;
        NoteStruct(elem->type);
        if (!NoJumps(f->body)) return false;
        Classify();
        if (!Body(f->body, true)) return false;
        // Each array stored to is read at the loop's index alone, and is not
        // the one the loop walks.
        for (auto v : stored)
            if (elsewhere[v] || v == it->vdef) return false;
        // No other array the loop reads can be one it stores to. An array
        // this function owns is reached only by its own name, where nothing
        // here bound a reference to it; two arrays of different element
        // types never share an element, whatever reaches them.
        for (auto v : stored) {
            auto owned = !v->isglobal && !v->isparam && v->type->kind == TY_ARRAY;
            auto distinct = [&](const VarDef *o) {
                if (o == v) return true;
                if (owned && (o->isglobal || o->isparam || (o->type && o->type->kind == TY_ARRAY)))
                    return true;
                auto oe = o->type ? ElemOf(o->type) : nullptr;
                return oe && cg.CT(oe) != cg.CT(ElemOf(v->type));
            };
            if (!distinct(it->vdef)) return false;
            for (auto &[o, n] : atidx) if (!distinct(o)) return false;
            for (auto &[o, n] : elsewhere) if (!distinct(o)) return false;
            for (auto o : stored) if (!distinct(o)) return false;
        }
        // Lanes pay where each one runs a loop of its own: a body of
        // straight-line arithmetic is the C compiler's to vectorize.
        return innerloop && lanefloat;
    }

    // ------------------------------------------------------------------
    // Emission.

    string gi;                                        // The C counter.
    CodeGen::ArrView view;                            // The walked array.
    unordered_map<const VarDef *, string> lnames;     // Lane locals' C names.
    unordered_map<const VarDef *, CodeGen::ArrView> views;
    unordered_map<const VarDef *, pair<bool, string>> savednames;

    string LaneCT(TypeExpr *t) {
        if (t->kind == TY_FLT) return "gs_l4d";
        return cat("gs_l4_", cg.CT(t));
    }

    void Leaves(TypeExpr *t, const string &path, vector<string> &out) {
        if (t->kind == TY_FLT) {
            out.push_back(path);
            return;
        }
        auto si = cg.SI(t);
        for (size_t i = 0; i < si->st->fields.size(); i++)
            Leaves(si->ftypes[i], cat(path, ".", cg.Sanitize(si->st->fields[i].name)), out);
    }
    vector<string> Leaves(TypeExpr *t) {
        vector<string> out;
        Leaves(t, "", out);
        return out;
    }

    // A VarDef's C name for this block alone; the old one comes back after.
    void Rename(const VarDef *v, const string &name) {
        if (!savednames.count(v)) {
            auto it = cg.vnames.find(v);
            savednames[v] = { it != cg.vnames.end(), it != cg.vnames.end() ? it->second : string() };
        }
        cg.vnames[v] = name;
    }
    void RestoreNames() {
        for (auto &[v, s] : savednames) {
            if (s.first) cg.vnames[v] = s.second;
            else cg.vnames.erase(v);
        }
    }

    string NewLane(TypeExpr *t) {
        auto n = cg.T();
        cg.L(LaneCT(t), " ", n, ";");
        return n;
    }

    // A uniform value, evaluated once.
    string Uniform(Node *n) {
        auto x = cg.GenX(n);
        auto t = cg.OperandT(n->exprtype);
        if (CodeGen::IsCName(x)) return x;
        auto s = cg.T();
        cg.L(cg.CT(t), " ", s, " = ", x, ";");
        return s;
    }

    // An operand of lane arithmetic: a lane value, or a uniform double the
    // vector extension spreads across the lanes.
    string Operand(Node *n) {
        if (Lane(n)) return LX(n);
        return cat("(double)(", Uniform(n), ")");
    }

    CodeGen::ArrView &ViewOf(const VarDef *v, Node *at) {
        auto it = views.find(v);
        if (it != views.end()) return it->second;
        auto lv = cg.GenLoc(at);
        if (lv.t->kind == TY_REF) cg.DerefLoc(lv);
        return views[v] = cg.ArrayView(lv);
    }

    string Elem(const CodeGen::ArrView &v, TypeExpr *et, const string &i) {
        return cat("(*(", cg.CT(et), " *)(", cg.ElemAddr(v, i), "))");
    }

    string Gather(const CodeGen::ArrView &v, TypeExpr *et) {
        auto t = NewLane(et);
        for (auto &p : Leaves(et)) {
            string s = cat(t, p, " = (gs_l4d){ ");
            for (auto l = 0; l < K; l++)
                Append(s, l ? ", " : "", Elem(v, et, cat("(", gi, " + ", l, ")")), p);
            cg.L(s, " };");
        }
        return t;
    }

    // The lanes a store reaches: those every condition around it holds in,
    // a gs_l4m of all ones or none per lane; empty for every lane.
    string mask;

    // A condition as such a mask.
    string MaskOf(Node *c) {
        string m;
        if (!Lane(c)) {
            m = cat("gs_l4m_splat(-(long long)(", Uniform(c), "))");
        } else if (auto u = Is<Unary>(c)) {
            m = cat("(~", MaskOf(u->child), ")");
        } else {
            auto b = static_cast<Binary *>(c);
            if (b->op == T_ANDAND || b->op == T_OROR) {
                auto l = MaskOf(b->left);
                auto r = MaskOf(b->right);
                m = cat("(", l, b->op == T_ANDAND ? " & " : " | ", r, ")");
            } else {
                static const unordered_map<int, const char *> ops = {
                    { T_LT, "<" }, { T_GT, ">" }, { T_LTEQ, "<=" }, { T_GTEQ, ">=" }, { T_EQ, "==" }, { T_NEQ, "!=" } };
                auto l = Operand(b->left);
                auto r = Operand(b->right);
                m = cat("(gs_l4m)(", l, " ", ops.at(b->op), " ", r, ")");
            }
        }
        auto t = cg.T();
        cg.L("gs_l4m ", t, " = ", m, ";");
        return t;
    }

    string Within(const string &m) { return mask.empty() ? m : cat("(", mask, " & ", m, ")"); }

    // Each lane's a where m holds there, its b where not.
    string Blend(const string &m, const string &a, const string &b) {
        return cat("((gs_l4d)(((gs_l4m)(", a, ") & ", m, ") | ((gs_l4m)(", b, ") & ~", m, ")))");
    }

    // dst, of type t, takes v in the lanes the mask holds in.
    void Masked(const string &dst, TypeExpr *t, const string &v) {
        for (auto &p : Leaves(t))
            cg.L(dst, p, " = ", Blend(mask, cat("(", v, ")", p), cat(dst, p)), ";");
    }

    // A branch's statements, and its value into res where res is given.
    void BranchCode(Node *b, const string &res, TypeExpr *t) {
        if (!b) return;
        if (auto x = Is<IfExpr>(b)) {
            if (res.empty()) S(x);
            else cg.L(res, " = ", LX(x), ";");
            return;
        }
        auto bl = Is<goose::Block>(b);
        if (!bl) {
            if (res.empty()) S(b);
            else Put(res, t, b);
            return;
        }
        for (auto st : bl->stmts) S(st);
        if (!bl->tail) return;
        if (res.empty()) S(bl->tail);
        else Put(res, t, bl->tail);
    }

    // Stores the lane value or the uniform one into dst, of type t.
    void Put(const string &dst, TypeExpr *t, Node *val) {
        if (Lane(val)) {
            cg.L(dst, " = ", LX(val), ";");
            return;
        }
        auto u = Uniform(val);
        for (auto &p : Leaves(t)) cg.L(dst, p, " = gs_l4d_splat((double)((", u, ")", p, "));");
    }

    static const char *OpStr(TType op) {
        switch (op) {
            case T_PLUS: case T_PLUSEQ: return "+";
            case T_MINUS: case T_MINUSEQ: return "-";
            case T_MUL: case T_MULEQ: return "*";
            default: return "/";
        }
    }

    // A lane value's C expression, after the statements it needs.
    string LX(Node *n) {
        switch (n->nkind) {
            case NK_Ident: return lnames.at(static_cast<Ident *>(n)->vdef);
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                auto si = cg.SI(cg.OperandT(d->obj->exprtype));
                return cat("(", LX(d->obj), ").", cg.Sanitize(si->st->fields[d->fieldidx].name));
            }
            case NK_Binary: {
                auto b = static_cast<Binary *>(n);
                if (b->exprtype->kind == TY_FLT) {
                    auto l = Operand(b->left);
                    auto r = Operand(b->right);
                    return cat("(", l, " ", OpStr(b->op), " ", r, ")");
                }
                // Elementwise (§6.1): each leaf of the result from the
                // operands' leaves, a scalar operand's value for every one.
                auto side = [&](Node *o, bool &scalar) {
                    scalar = cg.OperandT(o->exprtype)->kind != b->exprtype->kind;
                    if (Lane(o)) return LX(o);
                    auto u = Uniform(o);
                    return scalar ? cat("(double)(", u, ")") : u;
                };
                bool ls, rs;
                auto l = side(b->left, ls);
                auto r = side(b->right, rs);
                auto t = NewLane(b->exprtype);
                for (auto &p : Leaves(b->exprtype)) {
                    auto lp = ls ? l : cat("(", l, ")", p);
                    auto rp = rs ? r : cat("(", r, ")", p);
                    if (!ls && !Lane(b->left)) lp = cat("(double)(", lp, ")");
                    if (!rs && !Lane(b->right)) rp = cat("(double)(", rp, ")");
                    cg.L(t, p, " = (", lp, " ", OpStr(b->op), " ", rp, ");");
                }
                return t;
            }
            case NK_Unary: {
                auto u = static_cast<Unary *>(n);
                auto x = LX(u->child);
                if (u->exprtype->kind == TY_FLT) return cat("(-", x, ")");
                auto t = NewLane(u->exprtype);
                for (auto &p : Leaves(u->exprtype)) cg.L(t, p, " = (-(", x, ")", p, ");");
                return t;
            }
            case NK_AsCast: return LX(static_cast<AsCast *>(n)->child);
            case NK_Call: return cat("gs_l4d_sqrt(", LX(static_cast<Call *>(n)->args[0]), ")");
            case NK_Index: {
                auto x = static_cast<Index *>(n);
                return Gather(ViewOf(ArrayVar(x), x->obj), x->exprtype);
            }
            case NK_InlineBlock: {
                auto ib = static_cast<InlineBlock *>(n);
                if (NoJumps(ib->body)) {
                    for (auto s : ib->body->stmts) S(s);
                    return LX(ib->body->tail);
                }
                // Returning early: each lane's value from the return it takes,
                // the lanes still running in `mask`.
                auto t = n->exprtype;
                auto res = NewLane(t);
                for (auto &p : Leaves(t)) cg.L(res, p, " = gs_l4d_splat(0.0);");
                auto outer = mask;
                if (mask.empty()) {
                    mask = cg.T();
                    cg.L("gs_l4m ", mask, " = gs_l4m_splat(-1LL);");
                }
                RetEmit(ib->body, ib->sf, res, t);
                mask = outer;
                return res;
            }
            case NK_IfExpr: {
                auto x = static_cast<IfExpr *>(n);
                auto t = x->exprtype;
                auto res = NewLane(t);
                if (!Lane(x->cond)) {
                    cg.L("if (", Uniform(x->cond), ") {");
                    cg.ind++;
                    BranchCode(x->thenb, res, t);
                    cg.ind--;
                    cg.L("} else {");
                    cg.ind++;
                    BranchCode(x->elseb, res, t);
                    cg.ind--;
                    cg.L("}");
                    return res;
                }
                auto m = MaskOf(x->cond);
                auto outer = mask;
                auto a = NewLane(t), b = NewLane(t);
                mask = Within(m);
                BranchCode(x->thenb, a, t);
                mask = Within(cat("~", m));
                BranchCode(x->elseb, b, t);
                mask = outer;
                for (auto &p : Leaves(t)) cg.L(res, p, " = ", Blend(m, cat(a, p), cat(b, p)), ";");
                return res;
            }
            case NK_StructLit: {
                auto sl = static_cast<StructLit *>(n);
                auto t = NewLane(sl->exprtype);
                auto &fs = sl->sinst->st->fields;
                for (size_t i = 0; i < fs.size(); i++)
                    Put(cat(t, ".", cg.Sanitize(fs[i].name)), sl->sinst->ftypes[i], sl->InitFor((int)i));
                return t;
            }
            default: assert(false); return "";
        }
    }

    // A lane local's path, as an lvalue.
    string LanePath(Node *n) {
        if (auto id = Is<Ident>(n)) return lnames.at(id->vdef);
        auto d = static_cast<Dot *>(n);
        auto si = cg.SI(cg.OperandT(d->obj->exprtype));
        return cat(LanePath(d->obj), ".", cg.Sanitize(si->st->fields[d->fieldidx].name));
    }

    void S(Node *s) {
        switch (s->nkind) {
            case NK_VarDecl: {
                auto vd = static_cast<VarDecl *>(s);
                auto v = vd->defs[0];
                if (refs.count(v)) {
                    lnames[v] = lnames.at(static_cast<Ident *>(vd->inits[0])->vdef);
                    return;
                }
                if (lanev.count(v)) {
                    auto name = cg.Unique2(cat(cg.Sanitize(v->name), "l"));
                    cg.L(LaneCT(v->type), " ", name, ";");
                    lnames[v] = name;
                    Put(name, v->type, vd->inits[0]);
                    return;
                }
                auto x = cg.GenX(vd->inits[0]);
                auto name = cg.Unique2(cg.Sanitize(v->name));
                cg.L(cg.CT(v->type), " ", name, " = ", x, ";");
                Rename(v, name);
                return;
            }
            case NK_Assign: {
                auto a = static_cast<goose::Assign *>(s);
                if (auto x = Is<Index>(a->lval); x && IsVar(x->idx, idx)) {
                    auto &v = ViewOf(ArrayVar(x), x->obj);
                    auto et = x->exprtype;
                    string val;
                    if (Lane(a->rhs)) {
                        val = LX(a->rhs);
                    } else {
                        val = NewLane(et);
                        Put(val, et, a->rhs);
                    }
                    for (auto l = 0; l < K; l++)
                        for (auto &p : Leaves(et))
                            cg.L(Elem(v, et, cat("(", gi, " + ", l, ")")), p, " = (", val, ")", p, "[", l, "];");
                    return;
                }
                if (lanev.count(RootLocal(a->lval))) {
                    auto dst = LanePath(a->lval);
                    auto t = a->lval->exprtype;
                    if (mask.empty()) {
                        if (a->op == T_ASSIGN) Put(dst, t, a->rhs);
                        else cg.L(dst, " = (", dst, " ", OpStr(a->op), " ", Operand(a->rhs), ");");
                        return;
                    }
                    // Under a condition: the new value, then into the lanes
                    // the condition holds in.
                    auto v = NewLane(t);
                    if (a->op == T_ASSIGN) Put(v, t, a->rhs);
                    else cg.L(v, " = (", dst, " ", OpStr(a->op), " ", Operand(a->rhs), ");");
                    Masked(dst, t, v);
                    return;
                }
                auto x = cg.GenX(a->rhs);
                cg.L(cg.GenLoc(a->lval).s, " = ", x, ";");
                return;
            }
            case NK_ForLoop: {
                auto fl = static_cast<ForLoop *>(s);
                auto c = cg.T();
                if (auto r = Is<RangeExpr>(fl->iter)) {
                    auto lo = Uniform(r->lo);
                    auto hi = Uniform(r->hi);
                    cg.L("for (int64_t ", c, " = ", lo, "; ", c, " < ", hi, "; ", c, "++) {");
                    cg.ind++;
                    if (fl->vdef) Bind(fl->vdef, c);
                } else if (fl->iter->exprtype && fl->iter->exprtype->kind == TY_INT) {
                    auto hi = Uniform(fl->iter);
                    cg.L("for (int64_t ", c, " = 0; ", c, " < ", hi, "; ", c, "++) {");
                    cg.ind++;
                    if (fl->vdef) Bind(fl->vdef, c);
                } else {
                    auto &v = ViewOf(static_cast<Ident *>(fl->iter)->vdef, fl->iter);
                    auto et = fl->vdef->type;
                    cg.L("for (int64_t ", c, " = 0; ", c, " < (", v.len, "); ", c, "++) {");
                    cg.ind++;
                    auto name = cg.Unique2(cg.Sanitize(fl->vdef->name));
                    cg.L(cg.CT(et), " ", name, " = ", Elem(v, et, c), ";");
                    Rename(fl->vdef, name);
                    if (fl->idxdef) Bind(fl->idxdef, c);
                }
                Run(fl->body);
                cg.ind--;
                cg.L("}");
                return;
            }
            case NK_InlineBlock: Run(static_cast<InlineBlock *>(s)->body); return;
            case NK_IfExpr: {
                auto x = static_cast<IfExpr *>(s);
                if (!Lane(x->cond)) {
                    cg.L("if (", Uniform(x->cond), ") {");
                    cg.ind++;
                    BranchCode(x->thenb, "", nullptr);
                    cg.ind--;
                    if (x->elseb) {
                        cg.L("} else {");
                        cg.ind++;
                        BranchCode(x->elseb, "", nullptr);
                        cg.ind--;
                    }
                    cg.L("}");
                    return;
                }
                // Both branches, each storing into the lanes it is taken in.
                auto m = MaskOf(x->cond);
                auto outer = mask;
                mask = Within(m);
                BranchCode(x->thenb, "", nullptr);
                mask = outer;
                mask = Within(cat("~", m));
                BranchCode(x->elseb, "", nullptr);
                mask = outer;
                return;
            }
            case NK_Block:
                cg.L("{");
                cg.ind++;
                Run(static_cast<goose::Block *>(s));
                cg.ind--;
                cg.L("}");
                return;
            default: assert(false); return;
        }
    }

    // RetBlock's body into res, the lanes still running in mask: a return
    // takes its value into them and leaves none running; an if's branches
    // each run in the lanes it is taken in, and those still running after
    // either run on.
    void RetEmit(Node *b, SFunction *sf, const string &res, TypeExpr *t) {
        if (!b) return;
        if (auto r = Is<Return>(b)) {
            auto v = NewLane(t);
            Put(v, t, r->vals[0]);
            Masked(res, t, v);
            mask = cg.T();
            cg.L("gs_l4m ", mask, " = gs_l4m_splat(0LL);");
            return;
        }
        if (auto x = Is<IfExpr>(b); x && HasReturn(x, sf)) {
            auto m = MaskOf(x->cond);
            auto live = mask;
            auto mt = cg.T(), me = cg.T();
            cg.L("gs_l4m ", mt, " = (", live, " & ", m, ");");
            cg.L("gs_l4m ", me, " = (", live, " & ~", m, ");");
            // A branch no running lane takes is skipped: what it computes
            // is its own, and its lanes that run on are none.
            auto branch = [&](Node *br, const string &bm) {
                auto after = cg.T();
                cg.L("gs_l4m ", after, " = ", bm, ";");
                if (!br) return after;
                cg.L("if (gs_l4m_any(", bm, ")) {");
                cg.ind++;
                mask = bm;
                RetEmit(br, sf, res, t);
                cg.L(after, " = ", mask, ";");
                cg.ind--;
                cg.L("}");
                return after;
            };
            auto lt = branch(x->thenb, mt);
            auto le = branch(x->elseb, me);
            mask = cg.T();
            cg.L("gs_l4m ", mask, " = (", lt, " | ", le, ");");
            return;
        }
        auto bl = Is<goose::Block>(b);
        if (!bl) {
            S(b);
            return;
        }
        for (auto s : bl->stmts) RetEmit(s, sf, res, t);
        if (!bl->tail) return;
        if (Is<Return>(bl->tail) || HasReturn(bl->tail, sf)) {
            RetEmit(bl->tail, sf, res, t);
            return;
        }
        // The body's value, in the lanes that reach it.
        auto v = NewLane(t);
        Put(v, t, bl->tail);
        Masked(res, t, v);
    }

    // An integer binder of an inner loop, a copy of its C counter.
    void Bind(VarDef *v, const string &c) {
        auto name = cg.Unique2(cg.Sanitize(v->name));
        cg.L(cg.CT(v->type), " ", name, " = (", cg.CT(v->type), ")", c, ";");
        Rename(v, name);
    }

    void Run(goose::Block *b) {
        for (auto s : b->stmts) S(s);
        if (b->tail) S(b->tail);
    }

    // The block of lanes, ahead of the loop that runs what is left: `ctr`
    // counts the iterations, `v` views the walked array.
    void Emit(const string &ctr, const CodeGen::ArrView &v) {
        gi = ctr;
        view = v;
        cg.L("#if defined(GS_LANES)");
        cg.L("/* loop shape: ", K, " lanes (codegen_lanes.h) */");
        // Every other array loaded or stored at the index holds the block
        // too, or the block's iterations run one by one, to abort where
        // they would.
        string more = cat("(", v.len, ") - ", ctr, " >= ", K);
        auto walked = static_cast<Ident *>(f->iter)->vdef;
        for (auto s : indexed) {
            if (s == walked) continue;
            Ident at(f->line, s->name);
            at.vdef = const_cast<VarDef *>(s);
            at.exprtype = s->type;
            auto lv = cg.GenLoc(&at);
            if (lv.t->kind == TY_REF) cg.DerefLoc(lv);
            auto sv = cg.ArrayView(lv);
            views[s] = sv;
            Append(more, " && (", sv.len, ") - ", ctr, " >= ", K);
        }
        cg.L("for (; ", more, "; ", ctr, " += ", K, ") {");
        cg.ind++;
        for (auto t : structs) {
            string s = "typedef struct { ";
            auto si = cg.SI(t);
            for (size_t i = 0; i < si->st->fields.size(); i++)
                Append(s, LaneCT(si->ftypes[i]), " ", cg.Sanitize(si->st->fields[i].name), "; ");
            cg.L(s, "} ", LaneCT(t), ";");
        }
        auto e = Gather(v, elem->type);
        lnames[elem] = e;
        Run(f->body);
        cg.ind--;
        cg.L("}");
        cg.L("#endif");
        RestoreNames();
    }

    // The filter form's blocks, ahead of the loop that runs what is left:
    // the lanes' statements and tests, then, where any lane's tests hold,
    // `scalar`, which runs the block's iterations as the loop does.
    void EmitFilter(const string &ctr, const CodeGen::ArrView &v, const function<void()> &scalar) {
        gi = ctr;
        view = v;
        cg.L("#if defined(GS_LANES)");
        cg.L("/* loop shape: ", K, " lanes test, then one at a time (codegen_lanes.h) */");
        string more = cat("(", v.len, ") - ", ctr, " >= ", K);
        auto walked = static_cast<Ident *>(f->iter)->vdef;
        for (auto s : indexed) {
            if (s == walked) continue;
            Ident at(f->line, s->name);
            at.vdef = const_cast<VarDef *>(s);
            at.exprtype = s->type;
            auto lv = cg.GenLoc(&at);
            if (lv.t->kind == TY_REF) cg.DerefLoc(lv);
            auto sv = cg.ArrayView(lv);
            views[s] = sv;
            Append(more, " && (", sv.len, ") - ", ctr, " >= ", K);
        }
        cg.L("while (", more, ") {");
        cg.ind++;
        for (auto t : structs) {
            string s = "typedef struct { ";
            auto si = cg.SI(t);
            for (size_t i = 0; i < si->st->fields.size(); i++)
                Append(s, LaneCT(si->ftypes[i]), " ", cg.Sanitize(si->st->fields[i].name), "; ");
            cg.L(s, "} ", LaneCT(t), ";");
        }
        lnames[elem] = Gather(v, ElemT());
        for (auto s : prefix) S(s);
        string all;
        for (auto k : keys) {
            auto m = MaskOf(k);
            all = all.empty() ? m : cat("(", all, " & ", m, ")");
        }
        cg.L("if (!gs_l4m_any(", all, ")) {");
        cg.L("    ", ctr, " += ", K, ";");
        cg.L("    continue;");
        cg.L("}");
        RestoreNames();
        scalar();
        cg.ind--;
        cg.L("}");
        cg.L("#endif");
    }
};

// Whether a loop over an array runs in lanes; the caller has checked that
// its count is fixed at entry and its elements stay where they are.
inline bool CodeGen::LanesFor(ForLoop *f) {
    if (f->stripk || f->sumred) return false;
    LaneGen g(*this, f);
    return g.Plan();
}

inline void CodeGen::GenLanes(ForLoop *f, const string &ctr, const ArrView &v) {
    LaneGen g(*this, f);
    auto planned = g.Plan();
    assert(planned);
    (void)planned;
    g.Emit(ctr, v);
    lanesused = true;
}

// Gust: whether a loop over an array runs its tests in lanes (the filter
// form); the caller has checked as for LanesFor.
inline bool CodeGen::LaneFilterFor(ForLoop *f) {
    if (f->stripk || f->sumred) return false;
    LaneGen g(*this, f);
    return g.PlanFilter();
}

// The loop in the filter form: blocks of lanes, each running its
// iterations as the loop is where any lane's tests hold, then the rest.
// The two copies of the body share one loop scope, as GenBlocked's do.
inline void CodeGen::GenLaneFilter(ForLoop *f, const string &ctr, const ArrView &v,
                                   const function<void()> &bind, const Dst &d) {
    auto lp = OpenLoop(f->body, nullptr, { f->vdef, f->idxdef }, d);
    auto si = lp.si;
    cscopes[si].straight = StraightCode(f->body);
    {
        NameScope ns(*this);
        LaneGen g(*this, f);
        auto planned = g.PlanFilter();
        assert(planned);
        (void)planned;
        g.EmitFilter(ctr, v, [&]() {
            auto stop = T();
            L("for (int64_t ", stop, " = ", ctr, " + ", LaneGen::K, "; ", ctr, " < ", stop, "; ", ctr, "++) {");
            ind++;
            bind();
            GenBodyCopy(f->body, si);
            ind--;
            L("}");
        });
        lanesused = true;
    }
    cscopes[si].saves.clear();
    cscopes[si].usedcnt = false;
    cscopes[si].cntlbl = Lbl();
    L("for (; ", ctr, " < (", v.len, "); ", ctr, "++) {");
    ind++;
    bind();
    GenBodyCopy(f->body, si);
    ind--;
    L("}");
    CloseLoop(lp);
}

}  // namespace goose
