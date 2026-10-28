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
            case NK_InlineBlock: return Lane(static_cast<InlineBlock *>(n)->body->tail);
            case NK_StructLit: {
                for (auto &fi : static_cast<StructLit *>(n)->inits)
                    if (Lane(fi.val)) return true;
                return false;
            }
            default: return false;
        }
    }

    // Marks the locals a lane value reaches, through declarations and
    // assignments, until none is left to mark.
    void Classify() {
        if (elem) lanev.insert(elem);
        for (auto changed = true; changed;) {
            changed = false;
            function<void(Node *)> walk = [&](Node *n) {
                if (!n) return;
                if (auto vd = Is<VarDecl>(n)) {
                    for (size_t i = 0; i < vd->defs.size() && i < vd->inits.size(); i++)
                        if (!lanev.count(vd->defs[i]) && Lane(vd->inits[i]))
                            changed = lanev.insert(vd->defs[i]).second || changed;
                } else if (auto a = Is<Assign>(n)) {
                    if (auto v = RootLocal(a->lval); v && !lanev.count(v) && Lane(a->rhs))
                        changed = lanev.insert(v).second || changed;
                }
                n->Children(walk);
            };
            walk(f->body);
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

    bool Value(Node *n) {
        if (!n) return false;
        auto lane = Lane(n);
        if (lane && n->exprtype) {
            if (!Laneable(n->exprtype)) return false;
            NoteStruct(n->exprtype);
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
                if (Lane(x->idx)) return false;
                elsewhere[v]++;
                return Value(x->idx);
            }
            case NK_InlineBlock: {
                auto ib = static_cast<InlineBlock *>(n);
                return NoJumps(ib->body) && ib->body->tail && Body(ib->body, false) && Value(ib->body->tail);
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
            default: return false;
        }
    }

    bool Stmt(Node *s) {
        if (!s) return true;
        switch (s->nkind) {
            case NK_VarDecl: {
                auto vd = static_cast<VarDecl *>(s);
                if (vd->defs.size() != 1 || vd->inits.size() != 1 || vd->byref || vd->reusable) return false;
                auto v = vd->defs[0];
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
                if (fl->parallel || !fl->label.empty() || fl->byref) return false;
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
            default: return false;
        }
    }

    // A block's statements, and its tail where the block is a statement.
    bool Body(Block *b, bool tailstmt) {
        for (auto s : b->stmts)
            if (!Stmt(s)) return false;
        return !tailstmt || !b->tail || Stmt(b->tail);
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
                for (auto s : ib->body->stmts) S(s);
                return LX(ib->body->tail);
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
                    if (a->op == T_ASSIGN) Put(dst, a->lval->exprtype, a->rhs);
                    else cg.L(dst, " = (", dst, " ", OpStr(a->op), " ", Operand(a->rhs), ");");
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

}  // namespace goose
