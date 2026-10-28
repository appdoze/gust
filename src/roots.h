// Gust: root structs (docs/design/root_structs.md), lowered before the
// checker sees them.
//
// A `root struct` groups values under one owner that a struct could not
// hold together: any number of resizable fields, each growing on top of its
// own data stack. Each such field is stored apart, as a variable or
// parameter of its own beside the owner, named `owner.field` (a name no
// program can write): the owner itself is the struct of the other fields.
//
//     var parser = ParserState { .. };     // var parser.tokens: Token[>..] = [];
//                                          // var parser.work: i64[>..<] = [];
//                                          // var parser = ParserState { position: 0 };
//     fn parse(state: ParserState&, ...)   // fn parse(state: ParserState&,
//                                          //     state.tokens: Token[>..]&, state.work: i64[>..<]&, ...)
//     state.tokens.push(t);                // state.tokens.push(t);
//     parse(parser, input);                // parse(parser, parser.tokens, parser.work, input);
//
// So every field is a region of its own to the lifetime rules, as a
// variable is: growing or shrinking one never touches what points into
// another, of the same type or not, and each is an array the bounds-check
// elimination and the stack-top caches know. Nothing about the checker
// changes.
//
// A root value is a name: an owner, constructed by its literal where it is
// declared (local or global), a reference parameter `p: R&` or `const R&`,
// or a binding of either (`let q = p;`, `let q .= p;`). Through one, a
// field is read, a function taking `R&` called (also as `x.f(...)`), and
// that is all: a root value is never copied, assigned whole, returned,
// stored in an element, or handed to a builtin, and its type appears
// nowhere else (CheckTypes). A worker uses a root global as it uses any
// global, through a copy of each field it reaches.
//
// An ordinary struct may hold one by reference, as a parameter does: a
// field `st: R&` gains a reference field for each field of R stored apart,
// `st.field`, right after it, which its literal fills and `.=` rebinds.
//
//     struct Ctx { st: ParserState& }      // struct Ctx { st: ParserState&,
//                                          //     st.tokens: Token[>..]&, st.work: i64[>..<]& }
//     let c = Ctx { st: parser };          // let c = Ctx { st: parser,
//                                          //     st.tokens: parser.tokens, st.work: parser.work };
//     c.st.tokens.push(t);                 // c.st.tokens.push(t);    (the field `st.tokens`)
//
// Such a holder is an ordinary struct of references, copied, returned and
// stored as any is, the checker's store rule (§9.2) applying to each field.
// Which struct a field path reaches is known here only where the source
// writes the type, so `c.st` is a root value only for a name `c` declared
// Ctx, Ctx& or const Ctx&, or that a Ctx literal constructs (Known).
#pragma once
#include "builtins.h"

namespace goose {

struct RootLowering {
    Ast &ast;
    explicit RootLowering(Ast &a) : ast(a) {}

    struct Root {
        SStruct *st;
        vector<Field> all;    // As declared, the fields stored apart among them.
        vector<Field> comps;  // Those, in declaration order.
        bool IsComp(string_view f) const {
            for (auto &c : comps) if (c.name == f) return true;
            return false;
        }
    };
    unordered_map<SStruct *, Root> roots;

    // A name standing for a root value in the scope at hand.
    struct Binding {
        Root *root;
        vector<TypeExpr *> args;   // The struct's type arguments there.
        bool cq = false;           // Reached through `const R&`.
    };
    deque<Binding> bindings;

    // An ordinary struct with a field holding a root struct by reference.
    struct Holder {
        SStruct *st;
        vector<Field> src;          // As declared.
        struct Held {
            string_view name;
            Root *root;
            TypeExpr *type;         // R&, or const R&.
        };
        vector<Held> held;
        const Held *Find(string_view f) const {
            for (auto &h : held) if (h.name == f) return &h;
            return nullptr;
        }
    };
    unordered_map<SStruct *, Holder> holders;

    // A name whose type the source writes as a holder's. A `const Ctx&`
    // makes what its fields refer to no more const than any reference
    // field's pointee (§9.5).
    struct Known {
        Holder *h;
        vector<TypeExpr *> args;    // The holder's type arguments.
        unordered_map<string_view, Binding *> bound;   // Each held field's (HeldBinding).
    };
    deque<Known> knowns;

    // What a name stands for in the scope at hand: a root value, a holder
    // known by its type, or (neither) a name that shadows both.
    struct Named {
        Binding *root = nullptr;
        Known *known = nullptr;
    };
    // Innermost last.
    vector<unordered_map<string_view, Named>> scopes;
    unordered_map<VarDecl *, Binding *> globalroots;
    unordered_map<VarDecl *, Known *> globalknown;
    // The nested functions a scope declares, for calls of them.
    vector<unordered_map<string_view, vector<SFunction *>>> fnscopes;
    // The parameters every function was declared with, before its root
    // struct parameters gained their fields.
    unordered_map<SFunction *, vector<Param>> srcparams;
    // The types a root struct may be at the top of (CheckTypes).
    unordered_set<const TypeExpr *> allowed;
    // The bodies lowered: a parallel for's is its function's and its loop's.
    unordered_set<const Node *> done;

    void Error(Line l, const string &msg) {
        Diagnostic d;
        d.where = LocOf(l);
        d.message = msg;
        ast.Report(std::move(d));
    }
    void Error(const Node *n, const string &msg) {
        Diagnostic d;
        d.where = LocOf(n);
        d.message = msg;
        ast.Report(std::move(d));
    }

    // Whether a field of type t is resizable (§3.4), as the checker's
    // ClassOf finds before any instantiation: a growable array, or a struct
    // whose last field is one. A field of a type parameter's type is not.
    bool Resizable(const TypeExpr *t, int depth = 0) {
        if (!t || depth > 64) return false;
        if (t->kind == TY_ARRAY) return t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK;
        if (t->kind != TY_STRUCT) return false;
        auto st = t->struc->st;
        if (st->unknown || st->isroot) return false;
        for (auto i = st->fields.size(); i-- > 0;)
            if (!st->fields[i].ispad) return Resizable(st->fields[i].type, depth + 1);
        return false;
    }

    Root *RootType(const TypeExpr *t) {
        if (!t || t->kind != TY_STRUCT) return nullptr;
        auto it = roots.find(t->struc->st);
        return it == roots.end() ? nullptr : &it->second;
    }

    // `R&` or `const R&`: a plain reference to a root struct.
    Root *RootRef(const TypeExpr *t) {
        if (!t || !IsPlainRef(t)) return nullptr;
        return RootType(t->ref->sub);
    }

    // t with the struct's type parameters bound to args.
    TypeExpr *Subst(TypeExpr *t, const vector<GenericParam> &gs, const vector<TypeExpr *> &args) {
        if (!t || gs.empty()) return t;
        auto copy = [&](TypeExpr *o) {
            auto n = ast.NewType(o->kind, o->line);
            *n = *o;
            return n;
        };
        auto each = [&](const vector<TypeExpr *> &ts, bool &changed) {
            vector<TypeExpr *> out;
            for (auto c : ts) {
                out.push_back(Subst(c, gs, args));
                changed = changed || out.back() != c;
            }
            return out;
        };
        switch (t->kind) {
            case TY_GENERIC:
                for (size_t i = 0; i < gs.size() && i < args.size(); i++)
                    if (gs[i].name == t->named->name) return t->cq ? ast.ConstOf(args[i]) : args[i];
                return t;
            case TY_ARRAY: {
                auto s = Subst(t->arr->sub, gs, args);
                if (s == t->arr->sub) return t;
                auto n = copy(t);
                n->arr = ast.NewDetail<TypeArray>();
                *n->arr = *t->arr;
                n->arr->sub = s;
                return n;
            }
            case TY_REF: {
                auto s = Subst(t->ref->sub, gs, args);
                if (s == t->ref->sub) return t;
                auto n = copy(t);
                n->ref = ast.NewDetail<TypeRef>();
                *n->ref = *t->ref;
                n->ref->sub = s;
                return n;
            }
            case TY_SLICE: {
                auto s = Subst(t->sub, gs, args);
                if (s == t->sub) return t;
                auto n = copy(t);
                n->sub = s;
                return n;
            }
            case TY_STRUCT: {
                auto changed = false;
                auto a = each(t->struc->args, changed);
                if (!changed) return t;
                auto n = copy(t);
                n->struc = ast.NewDetail<TypeStruct>();
                n->struc->st = t->struc->st;
                n->struc->args = a;
                return n;
            }
            case TY_ENUM: {
                auto changed = false;
                auto a = each(t->enu->args, changed);
                if (!changed) return t;
                auto n = copy(t);
                n->enu = ast.NewDetail<TypeEnum>();
                n->enu->en = t->enu->en;
                n->enu->args = a;
                n->enu->varmode = t->enu->varmode;
                return n;
            }
            case TY_FN: {
                auto changed = false;
                auto a = each(t->fn->args, changed);
                auto r = each(t->fn->rets, changed);
                if (!changed) return t;
                auto n = copy(t);
                n->fn = ast.NewDetail<TypeFn>();
                *n->fn = *t->fn;
                n->fn->args = a;
                n->fn->rets = r;
                return n;
            }
            default: return t;
        }
    }

    TypeExpr *CompType(const Binding &b, const Field &c) { return Subst(c.type, b.root->st->generics, b.args); }

    TypeExpr *RefTo(TypeExpr *sub, bool cq, Line line) {
        auto t = ast.NewType(TY_REF, line);
        t->ref = ast.NewDetail<TypeRef>();
        t->ref->sub = sub;
        t->cq = cq;
        return t;
    }

    // The name a field of the root value `n` names spells (Ident::name).
    string_view CompName(string_view n, string_view field) { return ast.Intern(cat(n, ".", field)); }

    Binding *NewBinding(Root *r, const vector<TypeExpr *> &args, bool cq) {
        bindings.push_back({ r, args, cq });
        return &bindings.back();
    }

    // What a name stands for, and whether a local declares it.
    Named LookupName(const Ident *id, bool *local = nullptr) {
        if (local) *local = false;
        if (!SplitName(id->name, id->ns).qualified)
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                if (auto f = it->find(id->name); f != it->end()) {
                    if (local) *local = true;
                    return f->second;
                }
        Named n;
        if (auto g = ast.LookupGlobal(id->name, id->ns)) {
            if (auto f = globalroots.find(g); f != globalroots.end()) n.root = f->second;
            if (auto f = globalknown.find(g); f != globalknown.end()) n.known = f->second;
        }
        return n;
    }
    Binding *Lookup(const Ident *id) { return LookupName(id).root; }

    // A root value as the source names it: a name bound to one (`x`), or
    // a holder's field through a name whose type is the holder's (`c.st`).
    struct Path {
        Binding *b = nullptr;
        Ident *id = nullptr;        // The name, or the holder's.
        string_view field;          // The holder's field, for `c.st`.
        const Dot *dot = nullptr;   //   and where it is written.
        explicit operator bool() const { return b; }
    };
    string Text(const Path &p) { return p.field.empty() ? string(p.id->name) : cat(p.id->name, ".", p.field); }

    // The root value a node names, where it is one or `&` of one.
    Path PathOf(Node *n) {
        if (auto u = Is<Unary>(n); u && u->op == T_BITAND) n = u->child;
        if (auto id = Is<Ident>(n)) return { Lookup(id), id };
        auto d = Is<Dot>(n);
        auto id = d ? Is<Ident>(d->obj) : nullptr;
        auto k = id ? LookupName(id).known : nullptr;
        auto h = k ? k->h->Find(d->name) : nullptr;
        if (!h) return {};
        return { HeldBinding(*k, *h), id, d->name, d };
    }

    // The root value a held field of a known holder is.
    Binding *HeldBinding(Known &k, const Holder::Held &h) {
        auto &b = k.bound[h.name];
        if (b) return b;
        auto &gs = k.h->st->generics;
        vector<TypeExpr *> args;
        for (auto a : h.type->ref->sub->struc->args) args.push_back(Subst(a, gs, k.args));
        b = NewBinding(h.root, args, h.type->cq || h.type->ref->sub->cq);
        return b;
    }

    // The path again, as a node of its own.
    Node *Whole(const Path &p) {
        auto id = ast.New<Ident>(p.id->line, p.id->name, p.id->ns);
        id->end = p.id->end;
        if (!p.dot) return id;
        auto d = ast.New<Dot>(p.dot->line, id, p.field, p.dot->ns);
        d->end = p.dot->end;
        return d;
    }

    // The field f of the root value a path names: the variable or
    // parameter `x.f`, or the holder's field `c.st.f`.
    Node *Comp(const Path &p, string_view f) {
        if (!p.dot) {
            auto id = ast.New<Ident>(p.id->line, CompName(p.id->name, f), p.id->ns);
            id->end = p.id->end;
            return id;
        }
        auto id = ast.New<Ident>(p.id->line, p.id->name, p.id->ns);
        id->end = p.id->end;
        auto d = ast.New<Dot>(p.dot->line, id, CompName(p.field, f), p.dot->ns);
        d->end = p.dot->end;
        return d;
    }

    void Bind(string_view name, Binding *b) {
        if (!scopes.empty()) scopes.back()[name] = { b, nullptr };
    }
    void BindKnown(string_view name, Known *k) {
        if (!scopes.empty()) scopes.back()[name] = { nullptr, k };
    }

    // A holder of type t as the source writes it: Ctx, Ctx& or const Ctx&.
    Known *KnownType(const TypeExpr *t) {
        if (!t) return nullptr;
        if (IsPlainRef(t)) t = t->ref->sub;
        if (t->kind != TY_STRUCT) return nullptr;
        auto it = holders.find(t->struc->st);
        if (it == holders.end()) return nullptr;
        knowns.push_back({ &it->second, t->struc->args, {} });
        return &knowns.back();
    }

    // The holder a declaration's one name is known to be: its type's, or
    // the one its literal constructs.
    Known *KnownOf(VarDecl *vd) {
        if (vd->names.size() != 1) return nullptr;
        if (vd->type) return KnownType(vd->type);
        auto lit = vd->inits.size() == 1 ? Is<StructLit>(vd->inits[0]) : nullptr;
        return lit ? KnownType(lit->type) : nullptr;
    }

    // ------------------------------------------------------------------
    // Declarations.

    void Run() {
        for (auto st : ast.structs) {
            if (!st->isroot) continue;
            auto &r = roots[st];
            r.st = st;
            r.all = st->fields;
            vector<Field> kept;
            for (auto &f : st->fields) {
                if (!f.ispad && Resizable(f.type)) {
                    if (f.isconst)
                        Error(f.line, cat("resizable field ", f.name, " of root struct ", st->name,
                                          " cannot be a let field: it is stored apart, as a variable"));
                    r.comps.push_back(f);
                } else {
                    kept.push_back(f);
                }
            }
            st->fields = kept;
            st->rootfields = r.comps;
        }
        if (roots.empty()) return;
        for (auto st : ast.structs)
            if (!st->isroot) Hold(st);
        for (auto sf : ast.functions) ExpandParams(sf);
        // The globals, in their initialization order: an owner becomes its
        // fields' globals, then itself.
        scopes.emplace_back();
        fnscopes.emplace_back();
        vector<VarDecl *> globals;
        unordered_map<VarDecl *, vector<VarDecl *>> made;
        for (auto g : ast.globals) {
            if (auto b = OwnerOf(g)) {
                made[g] = LowerOwner(g, b, true);
                for (auto c : made[g]) globals.push_back(c);
                globalroots[g] = b;
            } else {
                for (auto &i : g->inits) i = Expr(i);
                if (g->names.size() == 1 && g->inits.size() == 1 && PathOf(g->inits[0]))
                    Error(g, "a global cannot be bound to a root struct value: name the owner itself");
                else if (auto k = KnownOf(g))
                    globalknown[g] = k;
            }
            globals.push_back(g);
        }
        // The declarations an owner became precede it among the topdecls
        // too.
        vector<Node *> tops;
        for (auto n : ast.topdecls) {
            if (auto vd = Is<VarDecl>(n))
                if (auto it = made.find(vd); it != made.end())
                    for (auto c : it->second) tops.push_back(c);
            tops.push_back(n);
        }
        ast.globals = globals;
        ast.topdecls = tops;
        for (auto sf : ast.functions)
            if (!sf->isnested) Fn(sf);
        scopes.pop_back();
        fnscopes.pop_back();
        CheckTypes();
    }

    // A root struct parameter `p: R&` gains a parameter for each field
    // stored apart, `p.field: F&`, right after it.
    void ExpandParams(SFunction *sf) {
        auto any = false;
        for (auto &p : sf->params) any = any || RootRef(p.type);
        if (!any) return;
        srcparams[sf] = sf->params;
        vector<Param> ps;
        for (auto &p : sf->params) {
            ps.push_back(p);
            auto r = RootRef(p.type);
            if (!r) continue;
            allowed.insert(p.type);
            allowed.insert(p.type->ref->sub);
            if (sf->isextern || sf->isexport)
                Error(p.line, cat("a root struct does not cross into C: parameter ", p.name, " of ",
                                  sf->name, " is a ", r->st->name, "&"));
            if (sf->isthread)
                Error(p.line, cat("a thread_fn takes no root struct: parameter ", p.name, " of ",
                                  sf->name, " is a ", r->st->name, "&"));
            if (p.defaultval)
                Error(p.line, cat("parameter ", p.name, " of ", sf->name,
                                  " takes a root struct, which has no default"));
            auto cq = p.type->cq || p.type->ref->sub->cq;
            Binding b { r, p.type->ref->sub->struc->args, cq };
            for (auto &c : r->comps) {
                Param q;
                q.name = CompName(p.name, c.name);
                q.line = p.line;
                q.type = RefTo(CompType(b, c), cq, p.line);
                q.rootcomp = true;
                ps.push_back(q);
            }
        }
        sf->params = ps;
    }

    // An ordinary struct's field `st: R&` gains a field for each of R's
    // stored apart, `st.field: F&`, right after it, as a parameter does.
    void Hold(SStruct *st) {
        auto any = false;
        for (auto &f : st->fields) any = any || (!f.ispad && RootRef(f.type));
        if (!any) return;
        auto &h = holders[st];
        h.st = st;
        h.src = st->fields;
        vector<Field> fs;
        for (auto &f : st->fields) {
            fs.push_back(f);
            auto r = f.ispad ? nullptr : RootRef(f.type);
            if (!r) continue;
            allowed.insert(f.type);
            allowed.insert(f.type->ref->sub);
            if (f.defaultval)
                Error(f.line, cat("field ", f.name, " of ", st->name, " holds a reference to a ", r->st->name,
                                  ", a root struct, which has no default"));
            h.held.push_back({ f.name, r, f.type });
            auto cq = f.type->cq || f.type->ref->sub->cq;
            Binding b { r, f.type->ref->sub->struc->args, cq };
            for (auto &c : r->comps) {
                Field g;
                g.name = CompName(f.name, c.name);
                g.line = f.line;
                g.type = RefTo(CompType(b, c), cq, f.line);
                g.isconst = f.isconst;
                g.rootcomp = true;
                fs.push_back(g);
            }
        }
        st->fields = fs;
    }

    const vector<Param> &SourceParams(SFunction *sf) {
        auto it = srcparams.find(sf);
        return it == srcparams.end() ? sf->params : it->second;
    }

    // The root an owner's declaration constructs, where it is one: its
    // literal's type is a root struct, or its own type is.
    Binding *OwnerOf(VarDecl *vd) {
        auto lit = vd->inits.size() == 1 ? Is<StructLit>(vd->inits[0]) : nullptr;
        auto r = lit ? RootType(lit->type) : nullptr;
        if (!r) r = RootType(vd->type);
        if (!r) return nullptr;
        auto st = r->st;
        allowed.insert(vd->type);
        if (lit) allowed.insert(lit->type);
        if (vd->names.size() != 1 || vd->inits.size() > 1) {
            Error(vd, cat("a root struct (", st->name, ") is declared one name at a time"));
            return nullptr;
        }
        if (!lit || (lit->type && !RootType(lit->type))) {
            Error(vd, cat(vd->names[0], " is a ", st->name, ", a root struct: construct it where it is declared, ",
                          vd->isvar ? "var " : "let ", vd->names[0], " = ", st->name, " { .. }"));
            return nullptr;
        }
        if (vd->byref) {
            Error(vd, cat("a root struct owner is constructed, not bound: ", vd->names[0], " = ", st->name,
                          " { .. }"));
            return nullptr;
        }
        if (vd->reusable) {
            Error(vd, cat("a root struct (", st->name, ") is not a reusable pool"));
            return nullptr;
        }
        auto t = lit->type ? lit->type : vd->type;
        if (!st->generics.empty() && t->struc->args.size() != st->generics.size()) {
            Error(vd, cat("give root struct ", st->name, "'s type arguments: ", st->name, "<...> { .. }"));
            return nullptr;
        }
        return NewBinding(r, t->struc->args, vd->isconst);
    }

    // An owner's declaration as its fields' declarations, in declaration
    // order, and then its own, of a literal of the fields left: an
    // initializer of one of those that calls something and comes before a
    // field stored apart is taken into a temporary in its place, so the
    // initializers run in the order the literal is declared in.
    vector<VarDecl *> LowerOwner(VarDecl *vd, Binding *b, bool global) {
        auto lit = Is<StructLit>(vd->inits[0]);
        auto &r = *b->root;
        auto name = vd->names[0];
        for (auto &fi : lit->inits) fi.val = Expr(fi.val);
        // Each initializer's field, by name or position.
        vector<int> target(lit->inits.size(), -1);
        auto pos = 0;
        for (size_t k = 0; k < lit->inits.size(); k++) {
            auto &fi = lit->inits[k];
            if (fi.name.empty()) {
                while (pos < (int)r.all.size() && r.all[pos].ispad) pos++;
                if (pos < (int)r.all.size()) target[k] = pos++;
            } else {
                for (size_t i = 0; i < r.all.size(); i++)
                    if (!r.all[i].ispad && r.all[i].name == fi.name) target[k] = (int)i;
            }
        }
        auto lastcomp = -1;
        for (size_t i = 0; i < r.all.size(); i++)
            if (r.IsComp(r.all[i].name) && !r.all[i].ispad) lastcomp = (int)i;
        auto calls = [&](Node *n) {
            auto found = false;
            function<void(Node *)> walk = [&](Node *c) {
                if (!c || found) return;
                if (Is<Call>(c)) { found = true; return; }
                c->Children(walk);
            };
            walk(n);
            return found;
        };
        vector<VarDecl *> out;
        auto rest = ast.New<StructLit>(lit->line, lit->type);
        rest->end = lit->end;
        rest->defaultall = lit->defaultall;
        rest->implicit = lit->implicit;
        for (size_t i = 0; i < r.all.size(); i++) {
            auto &f = r.all[i];
            if (f.ispad) continue;
            Node *init = nullptr;
            for (size_t k = 0; k < lit->inits.size(); k++)
                if (target[k] == (int)i) init = lit->inits[k].val;
            if (r.IsComp(f.name)) {
                if (!init && f.defaultval && lit->defaultall) init = f.defaultval->Clone(ast);
                if (!init) {
                    Error(lit, cat("field ", f.name, " of root struct ", r.st->name,
                                   f.defaultval ? " is not given: add it, or `..` for its default"
                                                : " is not given, and has no default"));
                    continue;
                }
                if (Is<SelfRef>(init)) Error(init, "a root struct's literal has no self");
                auto c = ast.New<VarDecl>(vd->line, vd->isvar);
                c->isconst = vd->isconst;
                c->isglobal = global;
                c->ns = vd->ns;
                c->names.push_back(CompName(name, f.name));
                c->type = CompType(*b, f);
                c->inits.push_back(init);
                c->end = vd->end;
                if (global) ast.NS(vd->ns).globalmap[c->names[0]] = c;
                out.push_back(c);
                continue;
            }
            if (!init) continue;
            FieldInit fi;
            fi.name = f.name;
            fi.val = init;
            if ((int)i < lastcomp && calls(init)) {
                auto t = ast.New<VarDecl>(vd->line, false);
                t->isglobal = global;
                t->ns = vd->ns;
                t->names.push_back(ast.Intern(cat(name, "#", f.name)));
                t->inits.push_back(init);
                if (global) ast.NS(vd->ns).globalmap[t->names[0]] = t;
                out.push_back(t);
                fi.val = ast.New<Ident>(init->line, t->names[0], vd->ns);
            }
            rest->inits.push_back(fi);
        }
        // An initializer naming no field is the checker's to report.
        for (size_t k = 0; k < lit->inits.size(); k++)
            if (target[k] < 0) rest->inits.push_back(lit->inits[k]);
        vd->inits[0] = rest;
        return out;
    }

    // ------------------------------------------------------------------
    // Bodies.

    void Fn(SFunction *sf) {
        if (!sf->body || done.count(sf->body)) return;
        scopes.emplace_back();
        fnscopes.emplace_back();
        auto &src = SourceParams(sf);
        for (auto &p : src) {
            if (auto r = RootRef(p.type))
                Bind(p.name, NewBinding(r, p.type->ref->sub->struc->args, p.type->cq || p.type->ref->sub->cq));
            else if (auto k = KnownType(p.type))
                BindKnown(p.name, k);
            else
                Bind(p.name, nullptr);
        }
        for (auto &p : sf->params)
            if (p.defaultval && !p.rootcomp) p.defaultval = Expr(p.defaultval);
        Stmts(sf->body);
        fnscopes.pop_back();
        scopes.pop_back();
    }

    void Stmts(Block *b) {
        if (!b || done.count(b)) return;
        done.insert(b);
        scopes.emplace_back();
        fnscopes.emplace_back();
        vector<Node *> out;
        for (auto s : b->stmts) {
            if (auto vd = Is<VarDecl>(s)) {
                for (auto d : Decl(vd)) out.push_back(d);
                continue;
            }
            if (auto a = Is<Assign>(s); a && a->op == T_DOTASSIGN) {
                // A binding's rebind, or a holder's field's (`c.st .= q`):
                // every field rebound with it.
                if (auto to = PathOf(a->lval)) {
                    auto from = PathOf(a->rhs);
                    if (!from || from.b->root != to.b->root) {
                        Error(a, cat(Text(to), " is bound to a ", to.b->root->st->name,
                                     ", a root struct: rebind it to another"));
                        continue;
                    }
                    if (from.b->cq && !to.b->cq)
                        Error(a, cat(Text(from), " is a const ", from.b->root->st->name, ", which ", Text(to),
                                     " would write"));
                    a->rhs = Whole(from);
                    a->lval = Whole(to);
                    out.push_back(a);
                    for (auto &c : to.b->root->comps) {
                        auto ca = ast.New<Assign>(a->line, T_DOTASSIGN, Comp(to, c.name), Comp(from, c.name));
                        ca->end = a->end;
                        out.push_back(ca);
                    }
                    continue;
                }
            }
            if (auto fd = Is<FnDecl>(s)) {
                fnscopes.back()[fd->sf->name].push_back(fd->sf);
                Fn(fd->sf);
                out.push_back(s);
                continue;
            }
            out.push_back(Expr(s));
        }
        b->stmts = out;
        if (b->tail) b->tail = Expr(b->tail);
        fnscopes.pop_back();
        scopes.pop_back();
    }

    // A local declaration: an owner's, a binding of a root value's, or any
    // other, whose names shadow the root values of theirs.
    vector<Node *> Decl(VarDecl *vd) {
        if (auto b = OwnerOf(vd)) {
            vector<Node *> out;
            for (auto c : LowerOwner(vd, b, false)) out.push_back(c);
            out.push_back(vd);
            Bind(vd->names[0], b);
            return out;
        }
        auto from = vd->inits.size() == 1 ? PathOf(vd->inits[0]) : Path {};
        if (from) {
            auto rb = from.b;
            auto st = rb->root->st;
            if (vd->names.size() != 1) {
                Error(vd, cat("a root struct (", st->name, ") is bound one name at a time"));
                return { vd };
            }
            if (vd->type && RootRef(vd->type) != rb->root) {
                Error(vd, cat(Text(from), " is a ", st->name, ", a root struct: ", vd->names[0],
                              " can only be bound to it by reference, ", vd->names[0], " .= ", Text(from)));
                return { vd };
            }
            if (vd->type) allowed.insert(vd->type), allowed.insert(vd->type->ref->sub);
            auto cq = rb->cq || (vd->type && (vd->type->cq || vd->type->ref->sub->cq)) || vd->isconst;
            // The type arguments the binding's type writes, else those of
            // what it binds.
            auto typed = vd->type && !vd->type->ref->sub->struc->args.empty();
            auto nb = NewBinding(rb->root, typed ? vd->type->ref->sub->struc->args : rb->args, cq);
            // A binding by reference, as of any non-fixed value (§3.8): the
            // struct of the other fields is fixed, so `.=` says so.
            vd->byref = true;
            vd->inits[0] = Whole(from);
            vector<Node *> out { vd };
            for (auto &c : rb->root->comps) {
                auto d = ast.New<VarDecl>(vd->line, vd->isvar);
                d->byref = true;
                d->names.push_back(CompName(vd->names[0], c.name));
                if (vd->type) d->type = RefTo(CompType(*nb, c), cq, vd->type->line);
                d->inits.push_back(Comp(from, c.name));
                d->end = vd->end;
                out.push_back(d);
            }
            Bind(vd->names[0], nb);
            return out;
        }
        if (vd->inits.size() == 1 && Is<IfExpr>(vd->inits[0])) {
            vector<Path> ch;
            if (Choices(vd->inits[0], ch, false)) return Select(vd, ch);
        }
        for (auto &i : vd->inits) i = Expr(i);
        for (auto n : vd->names) Bind(n, nullptr);
        if (auto k = KnownOf(vd)) BindKnown(vd->names[0], k);
        return { vd };
    }

    // The value a branch of an `if` gives, as the slot it is in: a block's
    // tail, or the branch itself (an `else if`, or a bare expression the
    // optimizer would leave).
    static Node *&BranchValue(Node *&b) {
        if (auto bl = Is<Block>(b)) return bl->tail;
        return b;
    }

    // The root values an `if` (with `else if`s) chooses among, in the order
    // of its branches, every branch giving one; with `number`, each is
    // replaced by its place in that order.
    bool Choices(Node *&v, vector<Path> &out, bool number) {
        if (!v) return false;
        if (auto x = Is<IfExpr>(v)) {
            Node *thenb = x->thenb;
            auto ok = x->elseb && Choices(BranchValue(thenb), out, number) &&
                      Choices(BranchValue(x->elseb), out, number);
            return ok;
        }
        auto p = PathOf(v);
        if (!p) return false;
        out.push_back(p);
        if (number) v = ast.New<IntLit>(v->line, (int64_t)out.size() - 1);
        return true;
    }

    // `let q = if c { a } else { b };` of root values a and b: the branch
    // taken, numbered once (q#sel), so its conditions and statements run
    // once and in order, then the owner's struct and each field bound to
    // the one that branch gives, as typed references take a branch's value.
    vector<Node *> Select(VarDecl *vd, vector<Path> &ch) {
        auto first = ch[0].b;
        auto r = first->root;
        auto st = r->st;
        auto cq = vd->isconst;
        for (auto &p : ch) {
            if (p.b->root != r || p.b->args.size() != first->args.size()) {
                Error(vd, cat("an if binding ", vd->names.empty() ? string_view() : vd->names[0],
                              " chooses among values of different root structs"));
                return { vd };
            }
            cq = cq || p.b->cq;
        }
        if (vd->names.size() != 1 || (vd->type && RootRef(vd->type) != r)) {
            Error(vd, cat("a root struct (", st->name, ") chosen by an if is bound one name at a time, by reference"));
            return { vd };
        }
        if (vd->type) {
            cq = cq || vd->type->cq || vd->type->ref->sub->cq;
            allowed.insert(vd->type);
            allowed.insert(vd->type->ref->sub);
        }
        auto line = vd->line;
        auto name = vd->names[0];
        auto sel = ast.Intern(cat(name, "#sel"));
        vector<Path> numbered;
        Choices(vd->inits[0], numbered, true);
        auto s = ast.New<VarDecl>(line, false);
        s->ns = vd->ns;
        s->names.push_back(sel);
        s->inits.push_back(Expr(vd->inits[0]));
        s->end = vd->end;
        vector<Node *> out { s };
        Bind(sel, nullptr);
        // if sel == 0 { leaf(0) } else if sel == 1 { leaf(1) } ... else { leaf(n - 1) }
        auto chain = [&](const function<Node *(const Path &)> &leaf) {
            Node *e = leaf(ch.back());
            for (auto i = (int)ch.size() - 2; i >= 0; i--) {
                auto c = ast.New<Binary>(line, T_EQ, ast.New<Ident>(line, sel, vd->ns), ast.New<IntLit>(line, (int64_t)i));
                auto thenb = ast.New<Block>(line);
                thenb->tail = leaf(ch[i]);
                auto elseb = ast.New<Block>(line);
                elseb->tail = e;
                e = ast.New<IfExpr>(line, c, thenb, elseb);
            }
            return e;
        };
        auto rt = ast.NewType(TY_STRUCT, line);
        rt->struc = ast.NewDetail<TypeStruct>();
        rt->struc->st = st;
        rt->struc->args = first->args;
        vd->type = RefTo(rt, cq, line);
        allowed.insert(vd->type);
        allowed.insert(rt);
        vd->byref = false;
        vd->inits[0] = chain([&](const Path &p) { return Whole(p); });
        out.push_back(vd);
        Binding b { r, first->args, cq };
        for (auto &c : r->comps) {
            auto d = ast.New<VarDecl>(line, vd->isvar);
            d->names.push_back(CompName(name, c.name));
            d->type = RefTo(CompType(b, c), cq, line);
            d->inits.push_back(chain([&](const Path &p) { return Comp(p, c.name); }));
            d->end = vd->end;
            out.push_back(d);
        }
        Bind(name, NewBinding(r, first->args, cq));
        return out;
    }

    // A value expression, lowered in place: what replaces it.
    Node *Expr(Node *n) {
        if (!n) return n;
        switch (n->nkind) {
            case NK_Ident: {
                auto id = static_cast<Ident *>(n);
                if (auto p = PathOf(id)) ValueUse(id, p);
                return n;
            }
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                if (auto p = PathOf(d->obj)) {
                    if (p.b->root->IsComp(d->name)) {
                        auto c = Comp(p, d->name);
                        c->end = d->end;
                        return c;
                    }
                    // A field of the struct the owner is, which the checker
                    // reads as any other.
                    return n;
                }
                // A holder's field, itself a root value.
                if (auto p = PathOf(d)) {
                    ValueUse(d, p);
                    return n;
                }
                d->obj = Expr(d->obj);
                return n;
            }
            case NK_Call: return CallExpr(static_cast<Call *>(n));
            case NK_Unary: {
                auto u = static_cast<Unary *>(n);
                if (u->op == T_BITAND)
                    if (auto p = PathOf(u->child)) {
                        ValueUse(u, p);
                        return n;
                    }
                u->child = Expr(u->child);
                return n;
            }
            case NK_Binary: {
                auto b = static_cast<Binary *>(n);
                b->left = Expr(b->left);
                b->right = Expr(b->right);
                return n;
            }
            case NK_Index: {
                auto x = static_cast<Index *>(n);
                x->obj = Expr(x->obj);
                x->idx = Expr(x->idx);
                return n;
            }
            case NK_SliceExpr: {
                auto x = static_cast<SliceExpr *>(n);
                x->obj = Expr(x->obj);
                x->lo = Expr(x->lo);
                x->hi = Expr(x->hi);
                return n;
            }
            case NK_AsCast: {
                auto x = static_cast<AsCast *>(n);
                x->child = Expr(x->child);
                return n;
            }
            case NK_RangeExpr: {
                auto x = static_cast<RangeExpr *>(n);
                x->lo = Expr(x->lo);
                x->hi = Expr(x->hi);
                return n;
            }
            case NK_ArrayLit: {
                auto x = static_cast<ArrayLit *>(n);
                for (auto &e : x->elems) e = Expr(e);
                x->fillval = Expr(x->fillval);
                x->fillcount = Expr(x->fillcount);
                x->capexpr = Expr(x->capexpr);
                return n;
            }
            case NK_StructLit: return Lit(static_cast<StructLit *>(n));
            case NK_Block: Stmts(static_cast<Block *>(n)); return n;
            case NK_IfExpr: {
                auto x = static_cast<IfExpr *>(n);
                x->cond = Expr(x->cond);
                Stmts(x->thenb);
                x->elseb = Expr(x->elseb);
                return n;
            }
            case NK_MatchExpr: {
                auto x = static_cast<MatchExpr *>(n);
                x->scrutinee = Expr(x->scrutinee);
                for (auto &arm : x->arms) {
                    arm.expansion = Expr(arm.expansion);
                    for (auto &pi : arm.pat.items) {
                        pi.lo = Expr(pi.lo);
                        pi.hi = Expr(pi.hi);
                    }
                    scopes.emplace_back();
                    if (!arm.pat.binder.empty()) Bind(arm.pat.binder, nullptr);
                    arm.body = Expr(arm.body);
                    scopes.pop_back();
                }
                return n;
            }
            case NK_EarlyBlock: Stmts(static_cast<EarlyBlock *>(n)->body); return n;
            case NK_While: {
                auto x = static_cast<While *>(n);
                x->cond = Expr(x->cond);
                Stmts(x->body);
                return n;
            }
            case NK_LoopExpr: Stmts(static_cast<LoopExpr *>(n)->body); return n;
            case NK_ForLoop: {
                auto x = static_cast<ForLoop *>(n);
                x->iter = Expr(x->iter);
                scopes.emplace_back();
                Bind(x->var, nullptr);
                if (!x->idxvar.empty()) Bind(x->idxvar, nullptr);
                Stmts(x->body);
                scopes.pop_back();
                return n;
            }
            case NK_ConstFor: {
                auto x = static_cast<ConstFor *>(n);
                for (auto &c : x->constants) c = Expr(c);
                scopes.emplace_back();
                for (auto nm : x->names) Bind(nm, nullptr);
                Stmts(x->body);
                for (auto &fi : x->inits) fi.val = Expr(fi.val);
                for (auto &arm : x->arms) {
                    arm.expansion = Expr(arm.expansion);
                    for (auto &pi : arm.pat.items) {
                        pi.lo = Expr(pi.lo);
                        pi.hi = Expr(pi.hi);
                    }
                    scopes.emplace_back();
                    if (!arm.pat.binder.empty()) Bind(arm.pat.binder, nullptr);
                    arm.body = Expr(arm.body);
                    scopes.pop_back();
                }
                scopes.pop_back();
                return n;
            }
            case NK_Return: {
                auto x = static_cast<Return *>(n);
                for (auto &v : x->vals) {
                    if (auto p = PathOf(v))
                        Error(v, cat(Text(p), " is a ", p.b->root->st->name,
                                     ", a root struct, which a function does not return"));
                    else
                        v = Expr(v);
                }
                return n;
            }
            case NK_Break: {
                auto x = static_cast<Break *>(n);
                x->val = Expr(x->val);
                return n;
            }
            case NK_FunVal: {
                auto x = static_cast<FunVal *>(n);
                scopes.emplace_back();
                if (x->params.empty() && !x->explicit_params) Bind("it", nullptr);
                for (auto &p : x->params) {
                    if (RootType(p.type) || RootRef(p.type))
                        Error(p.line, "a function value takes no root struct: name a function taking R&");
                    if (auto k = KnownType(p.type))
                        BindKnown(p.name, k);
                    else
                        Bind(p.name, nullptr);
                }
                Stmts(x->body);
                scopes.pop_back();
                return n;
            }
            case NK_VarDecl: {
                // Outside a block's statements: a for header's, say.
                auto x = static_cast<VarDecl *>(n);
                for (auto &i : x->inits) i = Expr(i);
                return n;
            }
            case NK_Assign: {
                auto x = static_cast<Assign *>(n);
                if (auto p = PathOf(x->lval)) {
                    auto st = p.b->root->st->name;
                    auto from = PathOf(x->rhs);
                    if (!p.dot)
                        Error(x, cat(Text(p), " is a ", st, ", a root struct, which is not assigned whole: assign its "
                                     "fields"));
                    else if (from)
                        Error(x, cat(Text(p), " refers to a ", st, ", a root struct, which is not assigned whole: "
                                     "rebind it, ", Text(p), " .= ", Text(from)));
                    else
                        Error(x, cat(Text(p), " refers to a ", st, ", a root struct, which is not assigned whole: "
                                     "rebind it to another with .="));
                    if (!from) x->rhs = Expr(x->rhs);
                    return n;
                }
                x->lval = Expr(x->lval);
                x->rhs = Expr(x->rhs);
                return n;
            }
            case NK_IncDec: {
                auto x = static_cast<IncDec *>(n);
                x->lval = Expr(x->lval);
                return n;
            }
            case NK_FnDecl: Fn(static_cast<FnDecl *>(n)->sf); return n;
            default: return n;
        }
    }

    // A root value where only a value would do.
    void ValueUse(const Node *at, const Path &p) {
        auto st = p.b->root->st->name;
        Error(at, cat(Text(p), " is a ", st, ", a root struct: name one of its fields, or pass it to a ",
                      st, "& parameter; it is not a value to copy, compare or print"));
    }

    // The functions a call's name reaches, nested ones first.
    vector<SFunction *> Candidates(string_view name, string_view ns) {
        if (!SplitName(name, ns).qualified)
            for (auto it = fnscopes.rbegin(); it != fnscopes.rend(); ++it)
                if (auto f = it->find(name); f != it->end()) return f->second;
        return ast.LookupFunctions(name, ns);
    }

    // The root struct each function a call's name reaches takes at
    // argument i, where all take the same one there: what is passed there
    // is then a root value, or the call is an error. And a parameter's
    // name, for the message.
    Root *TakesRoot(string_view name, string_view ns, size_t i, string_view *param = nullptr) {
        Root *r = nullptr;
        for (auto sf : Candidates(name, ns)) {
            auto &ps = SourceParams(sf);
            auto pr = i < ps.size() ? RootRef(ps[i].type) : nullptr;
            if (!pr || (r && pr != r)) return nullptr;
            r = pr;
            if (param) *param = ps[i].name;
        }
        return r;
    }

    // The holder some field of the name f holding r is a field of.
    SStruct *HolderOf(string_view f, const Root *r) {
        for (auto &[st, h] : holders)
            if (auto held = h.Find(f); held && held->root == r) return st;
        return nullptr;
    }

    // A value no name of a root value is, passed where fn takes the root
    // struct r: `e.st` of an expression e whose type is not written, or
    // any other.
    void NotRoot(Node *a, const Root *r, string_view fn, string_view param) {
        auto rn = r->st->name;
        auto d = Is<Dot>(a);
        if (auto st = d ? HolderOf(d->name, r) : nullptr) {
            auto hn = st->name;
            Error(a, cat(hn, ".", d->name, " holds a ", rn, ", a root struct, which ", fn,
                         " takes only from a name declared ", hn, ", ", hn, "& or const ", hn, "&, or one a ", hn,
                         " literal constructs: bind the ", hn, " to such a name first"));
            return;
        }
        Error(a, cat("parameter ", param, " of ", fn, " takes a ", rn, ", a root struct, which is passed by name: "
                     "the owner, a ", rn, "& parameter or a binding of one, or such a field of a name whose type "
                     "is written"));
    }

    // Whether a call's name is a variable's, whose function value the
    // checker calls before any function of the name.
    bool IsVariable(const Ident *id) {
        auto local = false;
        LookupName(id, &local);
        return local || ast.LookupGlobal(id->name, id->ns);
    }

    Node *CallExpr(Call *c) {
        // `x.f(...)` of a root value: `f(x, ...)`, which takes the fields
        // after x. A field of the owner's struct called is no method.
        if (auto d = Is<Dot>(c->callee)) {
            if (auto recv = PathOf(d->obj)) {
                auto b = recv.b;
                if (b->root->IsComp(d->name)) {
                    c->callee = Expr(d);
                } else if (none_of(b->root->st->fields.begin(), b->root->st->fields.end(),
                                   [&](const Field &f) { return f.name == d->name; })) {
                    // The method of the struct's own namespace, as UFCS
                    // reaches it (Ast::LookupMethods), unless a nested
                    // function of the name is in scope.
                    auto ns = b->root->st->ns;
                    auto local = false;
                    for (auto &fs : fnscopes) local = local || fs.count(d->name);
                    string_view name = d->name;
                    if (!local && !ns.empty())
                        if (auto nsp = ast.FindNS(ns); nsp && nsp->functionmap.count(d->name))
                            name = ast.Intern(cat(ns, "::", d->name));
                    auto callee = ast.New<Ident>(d->line, name, d->ns);
                    callee->end = d->end;
                    c->callee = callee;
                    c->args.insert(c->args.begin(), d->obj);
                }
            } else {
                // `e.st.f(...)` of a holder's field through an expression,
                // which f takes as a root value. Only a receiver that is
                // such a field: another's type may have a method f in its
                // own namespace, which the call reaches first.
                string_view param;
                auto od = Is<Dot>(d->obj);
                auto r = od ? TakesRoot(d->name, d->ns, 0, &param) : nullptr;
                if (r && HolderOf(od->name, r)) NotRoot(d->obj, r, d->name, param);
                d->obj = Expr(d->obj);
            }
        } else if (!Is<Ident>(c->callee)) {
            c->callee = Expr(c->callee);
        }
        auto callee = Is<Ident>(c->callee);
        auto named = callee && !IsVariable(callee);
        vector<Node *> args;
        for (size_t i = 0; i < c->args.size(); i++) {
            auto a = c->args[i];
            auto p = PathOf(a);
            if (!p) {
                string_view param;
                if (auto r = named ? TakesRoot(callee->name, callee->ns, i, &param) : nullptr)
                    NotRoot(a, r, callee->name, param);
                args.push_back(Expr(a));
                continue;
            }
            auto st = p.b->root->st;
            if (!callee) {
                Error(a, cat(Text(p), " is a ", st->name, ", a root struct, which a function value does not take"));
                args.push_back(a);
                continue;
            }
            auto cands = Candidates(callee->name, callee->ns);
            if (cands.empty()) {
                if (LookupBuiltin(GlobalLeaf(callee->name)))
                    Error(a, cat(Text(p), " is a ", st->name, ", a root struct, which ", callee->name,
                                 " does not take: pass it one of its fields"));
                args.push_back(a);   // An unknown function, which the checker reports.
                continue;
            }
            auto fits = false;
            for (auto sf : cands) {
                auto &ps = SourceParams(sf);
                fits = fits || (i < ps.size() && RootRef(ps[i].type) == p.b->root);
            }
            if (!fits) {
                Error(a, cat(Text(p), " is a ", st->name, ", a root struct: ", callee->name,
                             " takes it only by a parameter declared ", st->name, "& (or const ", st->name,
                             "&)"));
                args.push_back(a);
                continue;
            }
            args.push_back(Whole(p));
            for (auto &f : p.b->root->comps) args.push_back(Comp(p, f.name));
        }
        c->args = args;
        if (c->trailing) Expr(c->trailing);
        return c;
    }

    // A holder's literal gives each field holding a root struct a root
    // value, and so each field that one carries: `Ctx { st: p }` is
    // `Ctx { st: p, st.f: p.f, ... }`. Its initializers are then named,
    // as the fields gained come between those written.
    Node *Lit(StructLit *x) {
        auto it = x->type && x->type->kind == TY_STRUCT ? holders.find(x->type->struc->st) : holders.end();
        if (it == holders.end()) {
            for (auto &fi : x->inits) fi.val = Expr(fi.val);
            return x;
        }
        auto &h = it->second;
        auto hn = h.st->name;
        if (!x->inits.empty() && x->inits[0].name.empty()) {
            size_t pos = 0;
            for (auto &fi : x->inits) {
                while (pos < h.src.size() && h.src[pos].ispad) pos++;
                if (pos >= h.src.size()) {
                    Error(fi.val, cat("too many initializers for ", hn));
                    return x;
                }
                fi.name = h.src[pos++].name;
            }
        }
        vector<FieldInit> out;
        for (auto &fi : x->inits) {
            auto held = h.Find(fi.name);
            if (!held) {
                fi.val = Expr(fi.val);
                out.push_back(fi);
                continue;
            }
            auto rn = held->root->st->name;
            auto p = PathOf(fi.val);
            if (!p || p.b->root != held->root) {
                Error(fi.val, cat("field ", fi.name, " of ", hn, " holds a reference to a ", rn, ", a root struct, "
                                  "which it is given by name: the owner, a ", rn, "& parameter or a binding of one, "
                                  "or such a field of a name whose type is written"));
                out.push_back(fi);
                continue;
            }
            if (p.b->cq && !held->type->cq && !held->type->ref->sub->cq)
                Error(fi.val, cat(Text(p), " is a const ", rn, ", which field ", fi.name, " of ", hn, " would write"));
            fi.val = Whole(p);
            out.push_back(fi);
            for (auto &c : held->root->comps) {
                FieldInit ci;
                ci.name = CompName(fi.name, c.name);
                ci.val = Comp(p, c.name);
                out.push_back(ci);
            }
        }
        for (auto &held : h.held)
            if (none_of(out.begin(), out.end(), [&](const FieldInit &fi) { return fi.name == held.name; }))
                Error(x, cat("field ", held.name, " of ", hn, " is not given: it holds a reference to a ",
                             held.root->st->name, ", a root struct, which has no default"));
        x->inits = out;
        return x;
    }

    // ------------------------------------------------------------------
    // Where a root struct's type may be written: an owner's (its literal's
    // and declared type), a reference parameter's or binding's, `R&`, and
    // an ordinary struct's reference field's (Hold). Anywhere else -- a
    // root struct's field, an element, a result, a type argument, a cast,
    // an optional -- is an error, through aliases too: an alias's own type
    // is checked where it is used.
    void CheckTypes() {
        unordered_set<const TypeExpr *> child;
        auto kids = [&](const TypeExpr *t, auto &&f) {
            switch (t->kind) {
                case TY_ARRAY: f(t->arr->sub); break;
                case TY_REF: f(t->ref->sub); break;
                case TY_SLICE: f(t->sub); break;
                case TY_VARIANT: f(t->var->adt); break;
                case TY_STRUCT: for (auto a : t->struc->args) f(a); break;
                case TY_ENUM: for (auto a : t->enu->args) f(a); break;
                case TY_FN:
                    for (auto a : t->fn->args) f(a);
                    for (auto r : t->fn->rets) f(r);
                    break;
                case TY_GENERIC: case TY_UNRESOLVED:
                    if (t->named) for (auto a : t->named->args) f(a);
                    break;
                default: break;
            }
        };
        for (auto t : ast.alltypes) kids(t, [&](const TypeExpr *c) { if (c) child.insert(c); });
        for (auto a : ast.aliases) child.insert(a->type);
        // The root struct a type holds, at any depth.
        function<SStruct *(const TypeExpr *)> holds = [&](const TypeExpr *t) -> SStruct * {
            if (!t) return nullptr;
            if (auto r = RootType(t)) return r->st;
            SStruct *found = nullptr;
            kids(t, [&](const TypeExpr *c) { if (!found) found = holds(c); });
            return found;
        };
        // Where each type is written, for the message.
        unordered_map<const TypeExpr *, string> where;
        for (auto st : ast.structs)
            for (auto &f : (st->isroot ? roots[st].all : st->fields))
                if (f.type) where[f.type] = cat("field ", st->name, ".", f.name);
        for (auto en : ast.enums)
            for (auto &v : en->variants)
                for (auto &f : v.fields)
                    if (f.type) where[f.type] = cat("field ", en->name, ".", v.name, ".", f.name);
        for (auto sf : ast.functions) {
            for (auto &p : SourceParams(sf))
                if (p.type) where[p.type] = cat("parameter ", p.name, " of ", sf->name);
            for (auto r : sf->rets) where[r] = cat("the result of ", sf->name);
        }
        set<pair<int, int>> reported;
        for (auto t : ast.alltypes) {
            if (child.count(t)) continue;
            SStruct *st = nullptr;
            if (allowed.count(t)) {
                // R itself, or R&: its type arguments hold none.
                auto inner = t->kind == TY_REF ? t->ref->sub : t;
                kids(inner, [&](const TypeExpr *c) { if (!st) st = holds(c); });
            } else {
                st = holds(t);
            }
            if (!st || !reported.insert({ t->line.fileidx, t->line.line * 4096 + t->line.col }).second) continue;
            auto w = where.find(t);
            auto at = w == where.end() ? string("here") : cat("as the type of ", w->second);
            Error(t->line, cat(st->name, " is a root struct, which cannot be written ", at,
                               ": a root struct is the type of a variable its literal constructs (var x = ",
                               st->name, " { .. }), of a reference parameter (p: ", st->name, "&) or of an "
                               "ordinary struct's reference field (st: ", st->name, "&), and of nothing else"));
        }
    }
};

inline void LowerRootStructs(Ast &ast) {
    RootLowering(ast).Run();
}

}  // namespace goose
