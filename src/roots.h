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
// stored in a field or an element, or handed to a builtin, and its type
// appears nowhere else (CheckTypes). A worker uses a root global as it uses
// any global, through a copy of each field it reaches.
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
    // Innermost last; a null binding is a name that shadows one with no
    // root value.
    vector<unordered_map<string_view, Binding *>> scopes;
    unordered_map<VarDecl *, Binding *> globalroots;
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

    Binding *Lookup(const Ident *id) {
        if (!SplitName(id->name, id->ns).qualified)
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                if (auto f = it->find(id->name); f != it->end()) return f->second;
        if (auto g = ast.LookupGlobal(id->name, id->ns))
            if (auto f = globalroots.find(g); f != globalroots.end()) return f->second;
        return nullptr;
    }

    // The root value a node names, where it is a name bound to one or `&`
    // of one, and the name.
    Binding *RootOf(Node *n, Ident **name = nullptr) {
        if (auto u = Is<Unary>(n); u && u->op == T_BITAND) n = u->child;
        auto id = Is<Ident>(n);
        if (!id) return nullptr;
        if (name) *name = id;
        return Lookup(id);
    }

    void Bind(string_view name, Binding *b) {
        if (!scopes.empty()) scopes.back()[name] = b;
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
                if (g->names.size() == 1 && g->inits.size() == 1 && RootOf(g->inits[0]))
                    Error(g, "a global cannot be bound to a root struct value: name the owner itself");
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
                Ident *to = nullptr, *from = nullptr;
                auto lb = RootOf(a->lval, &to);
                if (lb) {
                    auto rb = RootOf(a->rhs, &from);
                    if (!rb || rb->root != lb->root) {
                        Error(a, cat(to->name, " is bound to a ", lb->root->st->name,
                                     ", a root struct: rebind it to another"));
                        continue;
                    }
                    if (rb->cq && !lb->cq)
                        Error(a, cat(from->name, " is a const ", rb->root->st->name, ", which ", to->name,
                                     " would write"));
                    a->rhs = ast.New<Ident>(from->line, from->name, from->ns);
                    a->lval = ast.New<Ident>(to->line, to->name, to->ns);
                    out.push_back(a);
                    for (auto &c : lb->root->comps) {
                        auto ca = ast.New<Assign>(a->line, T_DOTASSIGN,
                                                  ast.New<Ident>(to->line, CompName(to->name, c.name), to->ns),
                                                  ast.New<Ident>(from->line, CompName(from->name, c.name),
                                                                 from->ns));
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
        Ident *from = nullptr;
        auto rb = vd->inits.size() == 1 ? RootOf(vd->inits[0], &from) : nullptr;
        if (rb) {
            auto st = rb->root->st;
            if (vd->names.size() != 1) {
                Error(vd, cat("a root struct (", st->name, ") is bound one name at a time"));
                return { vd };
            }
            if (vd->type && RootRef(vd->type) != rb->root) {
                Error(vd, cat(from->name, " is a ", st->name, ", a root struct: ", vd->names[0],
                              " can only be bound to it by reference, ", vd->names[0], " .= ", from->name));
                return { vd };
            }
            if (vd->type) allowed.insert(vd->type), allowed.insert(vd->type->ref->sub);
            auto cq = rb->cq || (vd->type && (vd->type->cq || vd->type->ref->sub->cq)) || vd->isconst;
            // A binding by reference, as of any non-fixed value (§3.8): the
            // struct of the other fields is fixed, so `.=` says so.
            vd->byref = true;
            vd->inits[0] = ast.New<Ident>(from->line, from->name, from->ns);
            vector<Node *> out { vd };
            for (auto &c : rb->root->comps) {
                auto d = ast.New<VarDecl>(vd->line, vd->isvar);
                d->byref = true;
                d->names.push_back(CompName(vd->names[0], c.name));
                if (vd->type) d->type = RefTo(CompType(*rb, c), cq, vd->type->line);
                d->inits.push_back(ast.New<Ident>(from->line, CompName(from->name, c.name), from->ns));
                d->end = vd->end;
                out.push_back(d);
            }
            Bind(vd->names[0], NewBinding(rb->root, rb->args, cq));
            return out;
        }
        if (vd->inits.size() == 1 && Is<IfExpr>(vd->inits[0])) {
            vector<pair<Binding *, Ident *>> ch;
            if (Choices(vd->inits[0], ch, false)) return Select(vd, ch);
        }
        for (auto &i : vd->inits) i = Expr(i);
        for (auto n : vd->names) Bind(n, nullptr);
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
    bool Choices(Node *&v, vector<pair<Binding *, Ident *>> &out, bool number) {
        if (!v) return false;
        if (auto x = Is<IfExpr>(v)) {
            Node *thenb = x->thenb;
            auto ok = x->elseb && Choices(BranchValue(thenb), out, number) &&
                      Choices(BranchValue(x->elseb), out, number);
            return ok;
        }
        Ident *id = nullptr;
        auto b = RootOf(v, &id);
        if (!b) return false;
        out.push_back({ b, id });
        if (number) v = ast.New<IntLit>(v->line, (int64_t)out.size() - 1);
        return true;
    }

    // `let q = if c { a } else { b };` of root values a and b: the branch
    // taken, numbered once (q#sel), so its conditions and statements run
    // once and in order, then the owner's struct and each field bound to
    // the one that branch gives, as typed references take a branch's value.
    vector<Node *> Select(VarDecl *vd, vector<pair<Binding *, Ident *>> &ch) {
        auto r = ch[0].first->root;
        auto st = r->st;
        auto cq = vd->isconst;
        for (auto &[b, id] : ch) {
            if (b->root != r || b->args.size() != ch[0].first->args.size()) {
                Error(vd, cat("an if binding ", vd->names.empty() ? string_view() : vd->names[0],
                              " chooses among values of different root structs"));
                return { vd };
            }
            cq = cq || b->cq;
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
        vector<pair<Binding *, Ident *>> numbered;
        Choices(vd->inits[0], numbered, true);
        auto s = ast.New<VarDecl>(line, false);
        s->ns = vd->ns;
        s->names.push_back(sel);
        s->inits.push_back(Expr(vd->inits[0]));
        s->end = vd->end;
        vector<Node *> out { s };
        Bind(sel, nullptr);
        // if sel == 0 { leaf(0) } else if sel == 1 { leaf(1) } ... else { leaf(n - 1) }
        auto chain = [&](const function<Node *(Ident *)> &leaf) {
            Node *e = leaf(ch.back().second);
            for (auto i = (int)ch.size() - 2; i >= 0; i--) {
                auto c = ast.New<Binary>(line, T_EQ, ast.New<Ident>(line, sel, vd->ns), ast.New<IntLit>(line, (int64_t)i));
                auto thenb = ast.New<Block>(line);
                thenb->tail = leaf(ch[i].second);
                auto elseb = ast.New<Block>(line);
                elseb->tail = e;
                e = ast.New<IfExpr>(line, c, thenb, elseb);
            }
            return e;
        };
        auto rt = ast.NewType(TY_STRUCT, line);
        rt->struc = ast.NewDetail<TypeStruct>();
        rt->struc->st = st;
        rt->struc->args = ch[0].first->args;
        vd->type = RefTo(rt, cq, line);
        allowed.insert(vd->type);
        allowed.insert(rt);
        vd->byref = false;
        vd->inits[0] = chain([&](Ident *id) { return ast.New<Ident>(id->line, id->name, id->ns); });
        out.push_back(vd);
        Binding b { r, ch[0].first->args, cq };
        for (auto &c : r->comps) {
            auto d = ast.New<VarDecl>(line, vd->isvar);
            d->names.push_back(CompName(name, c.name));
            d->type = RefTo(CompType(b, c), cq, line);
            d->inits.push_back(chain([&](Ident *id) {
                return ast.New<Ident>(id->line, CompName(id->name, c.name), id->ns);
            }));
            d->end = vd->end;
            out.push_back(d);
        }
        Bind(name, NewBinding(r, ch[0].first->args, cq));
        return out;
    }

    // A value expression, lowered in place: what replaces it.
    Node *Expr(Node *n) {
        if (!n) return n;
        switch (n->nkind) {
            case NK_Ident: {
                auto id = static_cast<Ident *>(n);
                if (auto b = Lookup(id)) ValueUse(id, b);
                return n;
            }
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                Ident *id = nullptr;
                if (auto b = RootOf(d->obj, &id)) {
                    if (b->root->IsComp(d->name)) {
                        auto c = ast.New<Ident>(d->line, CompName(id->name, d->name), id->ns);
                        c->end = d->end;
                        return c;
                    }
                    // A field of the struct the owner is, which the checker
                    // reads as any other.
                    return n;
                }
                d->obj = Expr(d->obj);
                return n;
            }
            case NK_Call: return CallExpr(static_cast<Call *>(n));
            case NK_Unary: {
                auto u = static_cast<Unary *>(n);
                Ident *id = nullptr;
                if (u->op == T_BITAND)
                    if (auto b = RootOf(u->child, &id)) {
                        ValueUse(id, b);
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
            case NK_StructLit: {
                auto x = static_cast<StructLit *>(n);
                for (auto &fi : x->inits) fi.val = Expr(fi.val);
                return n;
            }
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
                    Ident *id = nullptr;
                    if (auto b = RootOf(v, &id))
                        Error(v, cat(id->name, " is a ", b->root->st->name,
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
                Ident *id = nullptr;
                if (auto b = RootOf(x->lval, &id)) {
                    Error(x, cat(id->name, " is a ", b->root->st->name,
                                 ", a root struct, which is not assigned whole: assign its fields"));
                    if (!RootOf(x->rhs)) x->rhs = Expr(x->rhs);
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
    void ValueUse(const Ident *id, const Binding *b) {
        auto st = b->root->st->name;
        Error(id, cat(id->name, " is a ", st, ", a root struct: name one of its fields, or pass it to a ",
                      st, "& parameter; it is not a value to copy, compare or print"));
    }

    // The functions a call's name reaches, nested ones first.
    vector<SFunction *> Candidates(const Ident *id) {
        if (!SplitName(id->name, id->ns).qualified)
            for (auto it = fnscopes.rbegin(); it != fnscopes.rend(); ++it)
                if (auto f = it->find(id->name); f != it->end()) return f->second;
        return ast.LookupFunctions(id->name, id->ns);
    }

    Node *CallExpr(Call *c) {
        // `x.f(...)` of a root value: `f(x, ...)`, which takes the fields
        // after x. A field of the owner's struct called is no method.
        if (auto d = Is<Dot>(c->callee)) {
            Ident *recv = nullptr;
            if (auto b = RootOf(d->obj, &recv)) {
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
                d->obj = Expr(d->obj);
            }
        } else if (!Is<Ident>(c->callee)) {
            c->callee = Expr(c->callee);
        }
        auto callee = Is<Ident>(c->callee);
        vector<Node *> args;
        for (size_t i = 0; i < c->args.size(); i++) {
            auto a = c->args[i];
            Ident *id = nullptr;
            auto b = RootOf(a, &id);
            if (!b) {
                args.push_back(Expr(a));
                continue;
            }
            auto st = b->root->st;
            if (!callee) {
                Error(a, cat(id->name, " is a ", st->name, ", a root struct, which a function value does not take"));
                args.push_back(a);
                continue;
            }
            auto cands = Candidates(callee);
            if (cands.empty()) {
                if (LookupBuiltin(GlobalLeaf(callee->name)))
                    Error(a, cat(id->name, " is a ", st->name, ", a root struct, which ", callee->name,
                                 " does not take: pass it one of its fields"));
                args.push_back(a);   // An unknown function, which the checker reports.
                continue;
            }
            auto fits = false;
            for (auto sf : cands) {
                auto &ps = SourceParams(sf);
                fits = fits || (i < ps.size() && RootRef(ps[i].type) == b->root);
            }
            if (!fits) {
                Error(a, cat(id->name, " is a ", st->name, ", a root struct: ", callee->name,
                             " takes it only by a parameter declared ", st->name, "& (or const ", st->name,
                             "&)"));
                args.push_back(a);
                continue;
            }
            auto ctl = ast.New<Ident>(id->line, id->name, id->ns);
            ctl->end = id->end;
            args.push_back(ctl);
            for (auto &f : b->root->comps) {
                auto ci = ast.New<Ident>(id->line, CompName(id->name, f.name), id->ns);
                ci->end = id->end;
                args.push_back(ci);
            }
        }
        c->args = args;
        if (c->trailing) Expr(c->trailing);
        return c;
    }

    // ------------------------------------------------------------------
    // Where a root struct's type may be written: an owner's (its literal's
    // and declared type) and a reference parameter's or binding's, `R&`.
    // Anywhere else -- a field, an element, a result, a type argument, a
    // cast, an optional -- is an error, through aliases too: an alias's
    // own type is checked where it is used.
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
                               st->name, " { .. }) or of a reference parameter (p: ", st->name, "&), and of "
                               "nothing else"));
        }
    }
};

inline void LowerRootStructs(Ast &ast) {
    RootLowering(ast).Run();
}

}  // namespace goose
