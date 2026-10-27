// Gust: `gust cimport header.h`, a C library's interface as a package. TinyCC
// parses the header and reports each declaration (tcc_set_decl_func,
// third_party/tinycc/tccgen.c); what the header itself declares becomes:
//
//   <name>/<name>.goose          `extern fn`s, structs laid out as C's, and
//                                the integer and string constants
//   <name>/<name>_cimport.c      the C each extern calls, which hands the
//                                library pointers where Goose passes slices
//   <name>/<name>.build          the sidecar: that C, and the library to link
//
// The package is an audited boundary like any hand-written one: its externs
// are listed by --externs and refused by --deny-externs, and nothing in it
// is more than C trusts. How C's shapes cross (docs/reference.md):
//
//   C                                  Goose
//   integers, float, double, _Bool     i8 .. u64, f32, f64, bool
//   char *, unsigned char *, and a     u8[:], whose data pointer C gets
//   void * its length follows
//   any other void *                   u64: user data, memory C allocated
//   const char *                       const u8[:], copied with a NUL
//   const void *, const uint8_t *      const u8[:]
//   const T *  (T a number)            const T[:]
//   T *        (T a number)            T[:], an out parameter an array of one
//   struct S *, struct S (by value)    S&, where S is laid out here
//   anything else behind a pointer     u64, a handle C owns, as is a
//                                      typedef of void's (libcurl's CURL *)
//   a function pointer                 u64, from c_callback(f)
//   a struct returned by value         an `out: S&` first parameter
//
// A function without a prototype, and one whose types have no Goose form (a
// union by value, long double, a struct with bitfields) are left out, with a
// comment saying so. A function returning `const char *` also gets a
// `<fn>_str` form returning its string, read with ffi::cstr. A variadic
// function is imported with no argument more, and as forms taking one more
// of each kind C's promotions leave (VarForm): curl_easy_setopt's options.
// A macro of a pointer constant (SQLite's SQLITE_TRANSIENT) is a u64.

#pragma once

namespace goose {

struct CImport {
    // One C type, parsed from the report's encoding.
    struct CTy {
        char k = '?';         // v b c C i u e f p a s U P ?
        int64_t n = 0;        // Bytes of a number, count of an array.
        bool cnst = false;
        shared_ptr<CTy> sub;  // A pointer's target, an array's element.
        string name;          // A struct's or union's.
        string spell;         // A function pointer's C spelling.
    };
    struct Field { string name; int64_t off; shared_ptr<CTy> ty; bool bits = false; };
    struct Struct { bool isunion = false; int64_t size = 0, align = 0; vector<Field> fields; bool complete = true; };
    struct Param { string name; shared_ptr<CTy> ty; };
    struct Func { string file, name; shared_ptr<CTy> ret; int kind = 0; vector<Param> params; };

    vector<Func> funcs;
    map<string, Struct> structs;
    vector<pair<string, string>> typedefs;      // name, encoded type
    vector<tuple<string, string, int64_t>> enums;   // file, name, value
    vector<pair<string, string>> macros;        // file, name
    vector<string> lines;

    static void Collect(void *self, const char *line) { ((CImport *)self)->lines.push_back(line); }

    static vector<string> Split(const string &s, char sep) {
        vector<string> out;
        size_t at = 0;
        for (;;) {
            auto p = s.find(sep, at);
            out.push_back(s.substr(at, p == string::npos ? string::npos : p - at));
            if (p == string::npos) return out;
            at = p + 1;
        }
    }

    static shared_ptr<CTy> Parse(const string &s, size_t &i) {
        auto t = make_shared<CTy>();
        while (i < s.size() && s[i] == 'K') {
            t->cnst = true;
            i++;
        }
        if (i >= s.size()) return t;
        auto num = [&] {
            int64_t v = 0;
            auto neg = i < s.size() && s[i] == '-';
            if (neg) i++;
            while (i < s.size() && isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
            return neg ? -v : v;
        };
        auto ident = [&] {
            string v;
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_')) v += s[i++];
            return v;
        };
        t->k = s[i++];
        switch (t->k) {
            case 'c': case 'C': case 'i': case 'u': case 'e': case 'f': t->n = num(); break;
            case 'p':
                if (s.compare(i, 2, "F{") == 0) {
                    t->k = 'P';
                    i += 2;
                    auto close = s.find('}', i);
                    t->spell = s.substr(i, close - i);
                    i = close == string::npos ? s.size() : close + 1;
                } else {
                    t->sub = Parse(s, i);
                }
                break;
            case 'a':
                t->n = num();
                if (i < s.size() && s[i] == ',') i++;
                t->sub = Parse(s, i);
                break;
            case 's': case 'U': t->name = ident(); break;
            case 'F': {
                auto close = s.find('}', i);
                i = close == string::npos ? s.size() : close + 1;
                break;
            }
            default: break;
        }
        return t;
    }
    static shared_ptr<CTy> Parse(const string &s) {
        size_t i = 0;
        return Parse(s, i);
    }

    void Read() {
        for (auto &l : lines) {
            auto f = Split(l, '\t');
            if (f.empty()) continue;
            if (f[0] == "f" && f.size() >= 5) {
                Func fn { f[1], f[2], Parse(f[3]), atoi(f[4].c_str()), {} };
                for (size_t k = 5; k < f.size(); k++) {
                    auto c = f[k].find(':');
                    fn.params.push_back({ f[k].substr(0, c), Parse(f[k].substr(c + 1)) });
                }
                funcs.push_back(std::move(fn));
            } else if (f[0] == "t" && f.size() >= 4) {
                typedefs.push_back({ f[2], f[3] });
            } else if (f[0] == "e" && f.size() >= 4) {
                enums.push_back({ f[1], f[2], strtoll(f[3].c_str(), nullptr, 10) });
            } else if (f[0] == "d" && f.size() >= 3) {
                macros.push_back({ f[1], f[2] });
            } else if (f[0] == "s" && f.size() >= 5) {
                auto &st = structs[f[2]];
                st.isunion = f[1] == "U";
                st.size = strtoll(f[3].c_str(), nullptr, 10);
                st.align = strtoll(f[4].c_str(), nullptr, 10);
                for (size_t k = 5; k < f.size(); k++) {
                    auto at = f[k].find('@'), c = f[k].find(':', at);
                    Field fd;
                    fd.name = f[k].substr(0, at);
                    fd.off = strtoll(f[k].substr(at + 1, c - at - 1).c_str(), nullptr, 10);
                    fd.bits = f[k].substr(c + 1) == "!";
                    fd.ty = Parse(f[k].substr(c + 1));
                    st.fields.push_back(fd);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Goose names and types.

    // A C name Goose can use: one the lexer reads as an identifier, else
    // with `_` after it.
    static bool IsIdent(const string &n) {
        if (n.empty()) return false;
        try {
            Lexer lx("", n.c_str());
            return lx.tok == T_IDENT && lx.attr.size() == n.size();
        } catch (CompileError &) {
            return false;
        }
    }
    static string GooseName(const string &n) { return IsIdent(n) ? n : cat(n, "_"); }

    // The Goose name of each struct: a typedef's where one names it alone
    // (`typedef struct z_stream_s {...} z_stream`), else its tag.
    map<string, string> sname;
    void NameStructs() {
        map<string, vector<string>> bytag;
        for (auto &[name, enc] : typedefs) {
            auto t = Parse(enc);
            if ((t->k == 's' || t->k == 'U') && !t->cnst) bytag[t->name].push_back(name);
        }
        for (auto &[tag, st] : structs) {
            auto it = bytag.find(tag);
            sname[tag] = GooseName(it != bytag.end() && it->second.size() == 1 ? it->second[0] : tag);
        }
    }
    // A struct's C spelling, for the glue: its tag, or its typedef where the
    // tag is anonymous.
    string CSpell(const string &tag) {
        if (tag.rfind("anon", 0) != 0) return cat("struct ", tag);
        return sname.count(tag) ? sname[tag] : tag;
    }

    static string Number(const CTy &t) {
        switch (t.k) {
            case 'b': return "bool";
            case 'c': return t.n == 1 ? "i8" : "";
            case 'C': return t.n == 1 ? "u8" : "";
            case 'i': case 'e': return t.n == 1 ? "i8" : t.n == 2 ? "i16" : t.n == 4 ? "i32" : t.n == 8 ? "i64" : "";
            case 'u': return t.n == 1 ? "u8" : t.n == 2 ? "u16" : t.n == 4 ? "u32" : t.n == 8 ? "u64" : "";
            case 'f': return t.n == 4 ? "f32" : t.n == 8 ? "f64" : "";
            default: return "";
        }
    }
    static string CNumber(const CTy &t) {
        auto g = Number(t);
        if (g == "bool") return "uint8_t";
        if (g == "f32") return "float";
        if (g == "f64") return "double";
        if (g.empty()) return "";
        return cat(g[0] == 'u' ? "uint" : "int", g.substr(1), "_t");
    }
    static bool Byte(const CTy &t) {
        return t.k == 'v' || ((t.k == 'c' || t.k == 'C' || t.k == 'i' || t.k == 'u') && t.n == 1);
    }

    // Whether a struct has a Goose layout: every field one with a Goose
    // type and no bitfields, nested structs too.
    // A struct C holds by value cannot hold itself, so one being decided
    // (3) is only ever reached through a pointer, a u64 here.
    map<string, int> mappable;   // 0 unknown, 1 yes, 2 no, 3 being decided
    bool Mappable(const string &tag) {
        auto &m = mappable[tag];
        if (m == 1 || m == 3) return true;
        if (m == 2) return false;
        auto it = structs.find(tag);
        if (it == structs.end() || it->second.isunion || it->second.size <= 0) {
            m = 2;
            return false;
        }
        m = 3;
        auto ok = true;
        for (auto &f : it->second.fields)
            ok = ok && !f.bits && !FieldType(*f.ty).empty();
        mappable[tag] = ok ? 1 : 2;
        return ok;
    }
    int64_t SizeOf(const CTy &t) {
        if (!Number(t).empty()) return t.k == 'b' ? 1 : t.n;
        if (t.k == 'p' || t.k == 'P') return 8;
        if (t.k == 'a') return t.sub ? t.n * SizeOf(*t.sub) : 0;
        if (t.k == 's' || t.k == 'U') return structs.count(t.name) ? structs[t.name].size : 0;
        return 0;
    }
    // A field's Goose type, or "" for one with none.
    string FieldType(const CTy &t) {
        if (auto g = Number(t); !g.empty()) return g;
        if (t.k == 'p' || t.k == 'P') return "u64";
        if (t.k == 'a' && t.sub && t.n > 0) {
            auto e = FieldType(*t.sub);
            return e.empty() ? "" : cat(e, "[", t.n, "]");
        }
        if (t.k == 's') return Mappable(t.name) ? sname[t.name] : "";
        if (t.k == 'U' && structs.count(t.name) && structs[t.name].size > 0)
            return cat("u8[", structs[t.name].size, "]");
        return "";
    }

    // ------------------------------------------------------------------
    // Output.

    string name, header;
    vector<string> files;   // The headers whose declarations are imported, by base name.
    string goose, glue;
    set<string> emitted;
    set<string> slicetypes;

    static string Base(const string &p) {
        auto s = p.find_last_of("/\\");
        return s == string::npos ? p : p.substr(s + 1);
    }
    bool Mine(const string &file) {
        auto b = Base(file);
        return std::find(files.begin(), files.end(), b) != files.end();
    }

    void EmitStruct(const string &tag) {
        if (emitted.count(tag) || !Mappable(tag)) return;
        emitted.insert(tag);
        auto &st = structs[tag];
        for (auto &f : st.fields)   // What it holds by value comes first.
            for (auto t = f.ty.get(); t; t = t->k == 'a' ? t->sub.get() : nullptr)
                if (t->k == 's') EmitStruct(t->name);
        string body;
        int64_t at = 0;
        set<string> seen;
        for (auto &f : st.fields) {
            if (f.off > at) Append(body, "    pad ", f.off - at, ",\n");
            auto fname = GooseName(f.name == "?" ? cat("f", f.off) : f.name);
            while (seen.count(fname)) fname += "_";
            seen.insert(fname);
            Append(body, "    ", fname, ": ", FieldType(*f.ty), ",\n");
            at = f.off + SizeOf(*f.ty);
        }
        if (st.size > at) Append(body, "    pad ", st.size - at, ",\n");
        Append(goose, "// ", CSpell(tag), ", ", st.size, " bytes as C lays it out.\nstruct ", sname[tag],
               " {\n", body, "}\n\n");
    }

    // A slice type for the glue, `T[:]` or `const T[:]` of a number T.
    string SliceC(const CTy &elem, bool cnst) {
        auto c = CNumber(elem);
        auto n = cat("gs_cimp_sl_", cnst ? "c" : "", c.substr(0, c.size() - 2));
        if (slicetypes.insert(n).second)
            Append(glue, "typedef struct { ", cnst ? "const " : "", c, " *data; int64_t len; } ", n, ";\n");
        return n;
    }

    // One parameter: its Goose type, the glue's parameter and the argument
    // it passes, what the glue does before and after the call. False where
    // it has no form here.
    struct Crossing { string gtype, cparam, carg, pre, post; };
    // lennext: whether an integer parameter follows, which makes a `void *`
    // a buffer of that length (gzread's) rather than a pointer C gave out
    // or a callback's user data (sqlite3_free's, sqlite3_exec's).
    bool Cross(const CTy &t, const string &pn, Crossing &x, string &why, bool lennext) {
        if (auto g = Number(t); !g.empty() && t.k != 'p') {
            x = { g, cat(CNumber(t), " ", pn), pn, "", "" };
            return true;
        }
        if (t.k == 'P') {
            // As a void *, which C compilers convert to the parameter's
            // function pointer type: TinyCC's spelling of that type is not
            // always C's (`enum <anonymous>`, `long long` for LP64's long).
            x = { "u64", cat("uint64_t ", pn), cat("(void *)(uintptr_t)", pn), "", "" };
            return true;
        }
        if (t.k == 's' && Mappable(t.name)) {
            EmitStruct(t.name);
            x = { cat(sname[t.name], "&"), cat("void *", pn), cat("*(", CSpell(t.name), " *)", pn), "", "" };
            return true;
        }
        if (t.k != 'p' || !t.sub) {
            why = "a parameter's type";
            return false;
        }
        auto &s = *t.sub;
        if (s.k == 'v' && !s.cnst && !lennext) {
            x = { "u64", cat("uint64_t ", pn), cat("(void *)(uintptr_t)", pn), "", "" };
            return true;
        }
        if (Byte(s) && !s.cnst) {
            x = { "u8[:]", cat("gs_cimp_bytes ", pn), cat("(void *)", pn, ".data"), "", "" };
            return true;
        }
        if ((s.k == 'c' || s.k == 'C') && s.n == 1 && s.cnst) {
            x = { "const u8[:]", cat("gs_cimp_bytes ", pn), cat("gs_z_", pn),
                  cat("    char *gs_z_", pn, " = gs_cimp_cstr(", pn, ");\n"), cat("    free(gs_z_", pn, ");\n") };
            return true;
        }
        if (Byte(s)) {
            x = { "const u8[:]", cat("gs_cimp_bytes ", pn), cat("(const void *)", pn, ".data"), "", "" };
            return true;
        }
        // C cannot say whether a pointer is to one number or to an array of
        // them, so it is a slice either way: an out parameter is an array of
        // one.
        if (!Number(s).empty()) {
            x = { cat(s.cnst ? "const " : "", Number(s), "[:]"), cat(SliceC(s, s.cnst), " ", pn),
                  cat("(void *)", pn, ".data"), "", "" };
            return true;
        }
        if (s.k == 's' && Mappable(s.name)) {
            EmitStruct(s.name);
            x = { cat(sname[s.name], "&"), cat("void *", pn), pn, "", "" };
            return true;
        }
        if (s.k == 'p' || s.k == 'P') {   // C writes a pointer back: a handle's slot.
            x = { "u64&", cat("uint64_t *", pn), cat("(void *)", pn), "", "" };
            return true;
        }
        x = { "u64", cat("uint64_t ", pn), cat("(void *)(uintptr_t)", pn), "", "" };
        return true;
    }

    // The argument a variadic function's form takes one more of, by its
    // suffix: an integer, a double, a handle or function pointer, a C string
    // (copied with a NUL, for the call only), a byte buffer C fills, and an
    // integer C writes (long *, curl_off_t *, or char ** read as an address).
    static constexpr const char *VARFORMS[] = { "_int", "_f64", "_ptr", "_str", "_buf", "_out" };
    Crossing VarForm(int k, const string &pn) {
        switch (k) {
            case 0: return { "i64", cat("int64_t ", pn), cat("(long long)", pn), "", "" };
            case 1: return { "f64", cat("double ", pn), pn, "", "" };
            case 2: return { "u64", cat("uint64_t ", pn), cat("(void *)(uintptr_t)", pn), "", "" };
            case 3:
                return { "const u8[:]", cat("gs_cimp_bytes ", pn), cat("gs_z_", pn),
                         cat("    char *gs_z_", pn, " = gs_cimp_cstr(", pn, ");\n"), cat("    free(gs_z_", pn, ");\n") };
            case 4: return { "u8[:]", cat("gs_cimp_bytes ", pn), cat("(void *)", pn, ".data"), "", "" };
            default: {
                CTy i64;
                i64.k = 'i';
                i64.n = 8;
                return { "i64[:]", cat(SliceC(i64, false), " ", pn), cat("(void *)", pn, ".data"), "", "" };
            }
        }
    }

    set<string> declared;   // Every function the header declares, so no form takes one's name.
    string lib;             // --lib, and what of the header it does not define:
    set<string> undefined;  // SQLite's snapshot functions, without SQLITE_ENABLE_SNAPSHOT.
    void EmitFunc(const Func &f) {
        if (undefined.count(f.name)) {
            Append(goose, "// ", f.name, ": not imported, since the library ", lib, " does not define it.\n");
            return;
        }
        if (f.kind == 2) {
            Append(goose, "// ", f.name, ": not imported, since it is declared without a prototype.\n");
            return;
        }
        if (f.kind != 1) return EmitForm(f, "", -1);
        Append(goose, "// ", f.name, " takes a variable number of arguments: imported with none more, and as ",
               f.name, "_int, _f64, _ptr, _str, _buf and _out, each with one more.\n");
        EmitForm(f, "", -1);
        for (int k = 0; k < 6; k++)
            if (!declared.count(cat(f.name, VARFORMS[k]))) EmitForm(f, VARFORMS[k], k);
    }

    // One extern of f, with one more argument of VarForm kind `extra` where
    // that is not -1.
    void EmitForm(const Func &f, const string &suffix, int extra) {
        auto gname = GooseName(f.name + suffix);
        auto cname = cat("gs_cimp_", name, "_", f.name, suffix);
        auto skip = [&](const string &why) {
            Append(goose, "// ", f.name, suffix, ": not imported, since ", why, ".\n");
        };
        vector<string> gparams, cparams, cargs;
        string pre, post, why;
        set<string> used;
        auto &r = *f.ret;
        auto retstruct = r.k == 's' && Mappable(r.name);
        if (retstruct) {
            EmitStruct(r.name);
            gparams.push_back(cat("out: ", sname[r.name], "&"));
            cparams.push_back("void *out");
            used.insert("out");
        }
        for (size_t i = 0; i < f.params.size(); i++) {
            auto &p = f.params[i];
            if (p.ty->k == 'v' && f.params.size() == 1) break;   // f(void)
            auto pn = GooseName(p.name.empty() ? cat("p", i) : p.name);
            while (used.count(pn)) pn += "_";
            used.insert(pn);
            Crossing x;
            auto lennext = i + 1 < f.params.size() && !Number(*f.params[i + 1].ty).empty() &&
                           f.params[i + 1].ty->k != 'b';
            if (!Cross(*p.ty, pn, x, why, lennext))
                return skip(cat("parameter ", p.name, " has no Goose form"));
            gparams.push_back(cat(pn, ": ", x.gtype));
            cparams.push_back(x.cparam);
            cargs.push_back(x.carg);
            pre += x.pre;
            post += x.post;
        }
        if (extra >= 0) {
            string pn = "v";
            while (used.count(pn)) pn += "_";
            auto x = VarForm(extra, pn);
            gparams.push_back(cat(pn, ": ", x.gtype));
            cparams.push_back(x.cparam);
            cargs.push_back(x.carg);
            pre += x.pre;
            post += x.post;
        }
        // The function, not a function-like macro of its name (zlib's
        // gzgetc is both): parenthesized, the name expands no macro.
        string gret, cret, call = cat("(", f.name, ")(");
        for (size_t i = 0; i < cargs.size(); i++) Append(call, i ? ", " : "", cargs[i]);
        call += ")";
        string body;
        if (retstruct) {
            cret = "void";
            body = cat(pre, "    *(", CSpell(r.name), " *)out = ", call, ";\n", post);
        } else if (r.k == 'v') {
            cret = "void";
            body = cat(pre, "    ", call, ";\n", post);
        } else {
            if (auto g = Number(r); !g.empty()) {
                gret = g;
                cret = CNumber(r);
                call = cat("(", cret, ")", call);
            } else if (r.k == 'p' || r.k == 'P') {
                gret = "u64";
                cret = "uint64_t";
                call = cat("(uint64_t)(uintptr_t)", call);
            } else {
                return skip("its result has no Goose form");
            }
            body = post.empty() ? cat(pre, "    return ", call, ";\n")
                                : cat(pre, "    ", cret, " gs_r = ", call, ";\n", post, "    return gs_r;\n");
        }
        string gp, cp;
        for (size_t i = 0; i < gparams.size(); i++) Append(gp, i ? ", " : "", gparams[i]);
        for (size_t i = 0; i < cparams.size(); i++) Append(cp, i ? ", " : "", cparams[i]);
        Append(goose, "extern fn ", gname, "(", gp, ")", gret.empty() ? "" : cat(" -> ", gret), " = \"", cname,
               "\";\n");
        Append(glue, cret, " ", cname, "(", cp.empty() ? "void" : cp, ") {\n", body, "}\n");
        // A C string result, read as a Goose string too (where `_str` is not
        // a variadic function's form).
        if (f.kind != 1 && r.k == 'p' && r.sub && (r.sub->k == 'c' || r.sub->k == 'C') && r.sub->n == 1) {
            string names;
            for (auto &gpar : gparams) Append(names, names.empty() ? "" : ", ", gpar.substr(0, gpar.find(':')));
            Append(goose, "fn ", f.name, "_str(", gp, ") -> u8[>..] { return ffi::cstr(", gname, "(", names,
                   ")); }\n");
        }
    }

    void Generate(const vector<pair<string, string>> &consts) {
        NameStructs();
        Append(glue, "/* Generated by `gust cimport ", Base(header), "`: the C each extern fn of ", name,
               ".goose calls. */\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"",
               Base(header), "\"\n\n",
               "typedef struct { uint8_t *data; int64_t len; } gs_cimp_bytes;\n\n",
               "static char *gs_cimp_cstr(gs_cimp_bytes s) {\n",
               "    char *z = (char *)malloc((size_t)s.len + 1);\n",
               "    if (!z) abort();\n",
               "    if (s.len) memcpy(z, s.data, (size_t)s.len);\n",
               "    z[s.len] = 0;\n",
               "    return z;\n}\n\n");
        set<string> seen;
        for (auto &[n, v] : consts) {
            auto gn = GooseName(n);
            if (!seen.insert(gn).second) continue;
            Append(goose, "let ", gn, " = ", v, ";\n");
        }
        if (!consts.empty()) goose += "\n";
        for (auto &f : funcs) declared.insert(f.name);
        set<string> done;
        for (auto &f : funcs) {
            if (!Mine(f.file) || !done.insert(f.name).second) continue;
            EmitFunc(f);
        }
        auto head = cat("// Generated by `gust cimport ", Base(header), "`: the C interface of ", Base(header),
                        ", a boundary whose\n// externs `gust --externs` lists. Regenerate it rather than edit "
                        "it.\nnamespace ", name, ";\n\n",
                        goose.find("ffi::cstr") != string::npos ? "import ffi;\n\n" : "");
        goose = head + goose;
    }
};

// The integer and string values of the header's object-like macros and
// enum constants: each macro is tried as a constant in a program that
// includes the header, many at once, halving the set where one is not.
struct CImportProbe {
    string libpath, prelude;
    vector<string> includes;
    vector<string> defines;

    TCCState *New(string &diags) {
        auto s = tcc_new();
        tcc_set_error_func(s, &diags, JitDiag);
        tcc_set_lib_path(s, libpath.c_str());
        for (auto &i : includes) tcc_add_include_path(s, i.c_str());
        for (auto &d : defines) {
            auto eq = d.find('=');
            tcc_define_symbol(s, d.substr(0, eq).c_str(), eq == string::npos ? "1" : d.substr(eq + 1).c_str());
        }
        tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
        return s;
    }

    // Kind 0: an integer (`long long`), 1: unsigned, 2: a string, 3: a
    // pointer constant, as a number. Fills vals for those in names that
    // compile as that kind.
    void Try(const vector<string> &names, int kind, map<string, string> &vals) {
        if (names.empty()) return;
        string src = prelude;
        for (size_t i = 0; i < names.size(); i++) {
            auto &n = names[i];
            if (kind == 2) {
                Append(src, "const char *gs_v", i, " = _Generic((", n, "), char *: (", n, "), const char *: (", n,
                       "));\n");
            } else if (kind == 3) {
                // What is left that converts to a number and is not a float
                // is a pointer; an enum value takes only a constant, so one
                // holding an address (`(void *)&f`, known only at run time)
                // does not compile.
                Append(src, "typedef char gs_t", i, "[_Generic((", n, "), float: -1, double: -1, long double: -1, ",
                       "default: 1)];\nenum { gs_e", i, " = (int)((unsigned long long)(", n, ") & 1) };\n",
                       "unsigned long long gs_v", i, " = (unsigned long long)(", n, ");\n");
            } else {
                // Floats and pointers are no integer constants: they leave the
                // _Generic without a match, which does not compile. An
                // unsigned 64-bit one is tried as unsigned, where it fits.
                auto types = kind ? string("unsigned long: 0, unsigned long long: 0")
                                  : string("char: 0, signed char: 0, unsigned char: 0, short: 0, "
                                           "unsigned short: 0, int: 0, unsigned: 0, long: 0, long long: 0");
                Append(src, kind ? "unsigned long long" : "long long", " gs_v", i, " = _Generic((", n, ") + 0, ",
                       types, ") + (", n, ");\n");
            }
        }
        string diags;
        auto s = New(diags);
        auto ok = tcc_compile_string(s, src.c_str()) >= 0 && tcc_relocate(s) >= 0;
        if (ok) {
            for (size_t i = 0; i < names.size(); i++) {
                auto p = tcc_get_symbol(s, cat("gs_v", i).c_str());
                if (!p) continue;
                if (kind == 2) {
                    auto str = *(const char **)p;
                    if (!str) continue;
                    string lit = "\"";
                    for (auto c = str; *c; c++) {
                        char b[8];
                        if (*c == '"' || *c == '\\') Append(lit, "\\", string(1, *c));
                        else if ((unsigned char)*c < 32 || (unsigned char)*c >= 127) {
                            snprintf(b, sizeof b, "\\x%02x", (unsigned char)*c);
                            lit += b;
                        } else lit += *c;
                    }
                    vals[names[i]] = lit + "\"";
                } else if (kind == 1 || kind == 3) {
                    vals[names[i]] = cat(*(unsigned long long *)p);
                } else {
                    vals[names[i]] = cat(*(long long *)p);
                }
            }
        }
        tcc_delete(s);
        if (ok || names.size() == 1) return;
        auto half = names.size() / 2;
        Try(vector<string>(names.begin(), names.begin() + half), kind, vals);
        Try(vector<string>(names.begin() + half, names.end()), kind, vals);
    }
};

inline int RunCimport(const string &exe, const vector<string> &args) {
    CImport ci;
    string outdir = ".", lib;
    vector<string> includes, defines, sources;
    for (size_t i = 0; i < args.size(); i++) {
        auto &a = args[i];
        auto next = [&]() -> string {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "gust cimport: %s takes a value\n", a.c_str());
                exit(2);
            }
            return args[++i];
        };
        if (a == "--name") ci.name = next();
        else if (a == "--lib") lib = next();
        else if (a == "--out") outdir = next();
        else if (a == "--also") for (auto &f : CImport::Split(next(), ',')) ci.files.push_back(f);
        else if (a == "--source") sources.push_back(next());
        else if (a == "-I") includes.push_back(next());
        else if (a.rfind("-I", 0) == 0) includes.push_back(a.substr(2));
        else if (a == "-D") defines.push_back(next());
        else if (a.rfind("-D", 0) == 0) defines.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "gust cimport: unknown option %s (gust help cimport)\n", a.c_str());
            return 2;
        } else if (ci.header.empty()) ci.header = a;
        else {
            fprintf(stderr, "gust cimport: one header at a time, not also %s\n", a.c_str());
            return 2;
        }
    }
    if (ci.header.empty()) {
        fprintf(stderr, "usage: gust cimport header.h [--name n] [--lib l] [--also a.h,b.h] [-I dir] [-D x=y] "
                        "[--out dir]\n");
        return 2;
    }
    ci.files.push_back(CImport::Base(ci.header));
    if (ci.name.empty()) {
        auto b = CImport::Base(ci.header);
        ci.name = b.substr(0, b.find('.'));
        for (auto &c : ci.name) if (!isalnum((unsigned char)c) && c != '_') c = '_';
    }
    if (!CImport::IsIdent(ci.name)) {
        fprintf(stderr, "gust cimport: %s is not a namespace name: give one with --name\n", ci.name.c_str());
        return 2;
    }
    // A header named by a path is found from its directory.
    auto dir = DirOf(ci.header);
    if (!dir.empty()) includes.push_back(dir);
    CImportProbe probe;
    probe.libpath = JitLibPath(DirOf(exe));
    probe.includes = includes;
    probe.defines = defines;
    probe.prelude = cat("#include \"", CImport::Base(ci.header), "\"\n");
    string diags;
    auto s = probe.New(diags);
    tcc_set_decl_func(s, &ci, CImport::Collect);
    if (tcc_compile_string(s, probe.prelude.c_str()) < 0) {
        fprintf(stderr, "gust cimport: %s does not compile:\n%s", ci.header.c_str(), diags.c_str());
        tcc_delete(s);
        return 1;
    }
    tcc_delete(s);
    ci.Read();
    // What the library defines, where TinyCC can load it (a shared library
    // where it runs programs): a header may declare more, which no program
    // could link. Each function's address in one table, each one the
    // library lacks an unresolved reference.
    ci.lib = lib;
    if (!lib.empty()) {
        string ldiags;
        auto ls = probe.New(ldiags);
        if (tcc_add_library(ls, lib.c_str()) == 0) {
            auto src = cat(probe.prelude, "void *gs_cimp_defined[] = {\n");
            set<string> seen;
            for (auto &f : ci.funcs)
                if (ci.Mine(f.file) && seen.insert(f.name).second) Append(src, "    (void *)&", f.name, ",\n");
            src += "    0 };\n";
            if (tcc_compile_string(ls, src.c_str()) >= 0 && tcc_relocate(ls) < 0) {
                const string mark = "unresolved reference to '";
                for (auto p = ldiags.find(mark); p != string::npos; p = ldiags.find(mark, p + 1)) {
                    auto b = p + mark.size(), e = ldiags.find('\'', b);
                    if (e != string::npos) ci.undefined.insert(ldiags.substr(b, e - b));
                }
            }
        }
        tcc_delete(ls);
    }
    // The constants: the header's own macros, then its enum constants.
    vector<string> names;
    set<string> named;
    for (auto &[file, n] : ci.macros)
        if (ci.Mine(file) && n.rfind("__", 0) != 0 && named.insert(n).second) names.push_back(n);
    map<string, string> ints, uns, strs;
    probe.Try(names, 0, ints);
    vector<string> rest;
    for (auto &n : names) if (!ints.count(n)) rest.push_back(n);
    probe.Try(rest, 1, uns);
    vector<string> rest2;
    for (auto &n : rest) if (!uns.count(n)) rest2.push_back(n);
    probe.Try(rest2, 2, strs);
    vector<string> rest3;
    for (auto &n : rest2) if (!strs.count(n)) rest3.push_back(n);
    map<string, string> ptrs;
    probe.Try(rest3, 3, ptrs);
    vector<pair<string, string>> consts;
    for (auto &n : names) {
        if (ints.count(n)) consts.push_back({ n, ints[n] });
        else if (uns.count(n)) consts.push_back({ n, uns[n] });
        else if (strs.count(n)) consts.push_back({ n, strs[n] });
        else if (ptrs.count(n)) consts.push_back({ n, cat(ptrs[n], " as u64") });
    }
    for (auto &[file, n, v] : ci.enums)
        if (ci.Mine(file) && !ints.count(n)) consts.push_back({ n, cat(v) });
    ci.Generate(consts);
    auto pkg = cat(outdir, "/", ci.name);
    std::error_code ec;
    filesystem::create_directories(pkg, ec);
    auto write = [&](const string &file, const string &text) {
        auto f = fopen(cat(pkg, "/", file).c_str(), "wb");
        if (!f) {
            fprintf(stderr, "gust cimport: cannot write %s/%s\n", pkg.c_str(), file.c_str());
            exit(1);
        }
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    };
    write(cat(ci.name, ".goose"), ci.goose);
    write(cat(ci.name, "_cimport.c"), ci.glue);
    // C the package compiles as its own, such as a library shipped as one
    // source file, copied into it.
    auto csrcs = cat("\"", ci.name, "_cimport.c\"");
    for (auto &src : sources) {
        string text;
        if (!LoadFile(src, text)) {
            fprintf(stderr, "gust cimport: cannot read %s\n", src.c_str());
            return 1;
        }
        write(CImport::Base(src), text);
        Append(csrcs, ", \"", CImport::Base(src), "\"");
    }
    string build = cat("// The sidecar of ", ci.name, ".goose, generated by gust cimport.\nc_sources = [",
                       csrcs, "];\n");
    if (!includes.empty()) {
        build += "c_include = [";
        for (size_t k = 0; k < includes.size(); k++)
            Append(build, k ? ", " : "", "\"", filesystem::absolute(includes[k], ec).generic_string(), "\"");
        build += "];\n";
    }
    if (!lib.empty()) Append(build, "c_libs = [\"", lib, "\"];\n");
    write(cat(ci.name, ".build"), build);
    auto nfn = 0, nskip = 0;
    for (auto p = ci.goose.find("extern fn "); p != string::npos; p = ci.goose.find("extern fn ", p + 1)) nfn++;
    for (auto p = ci.goose.find("not imported"); p != string::npos; p = ci.goose.find("not imported", p + 1))
        nskip++;
    printf("wrote %s: %d function(s), %d left out, %d constant(s), %d struct(s)\n", pkg.c_str(), nfn, nskip,
           (int)consts.size(), (int)ci.emitted.size());
    return 0;
}

}  // namespace goose
