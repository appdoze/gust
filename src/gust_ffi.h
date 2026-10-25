// Gust: what `--externs` reports about the C an extern fn meets beyond its
// name: the layout of the values it passes. Goose lays a struct out packed,
// in declaration order with only the `pad`s written in it (§3.2), and C lays
// the same fields out at their natural alignment, so the two agree only where
// the pads stand where C pads. This works out both layouts for the shapes
// that may cross (§7.10) and says where they differ. It assumes the C side
// declares the same fields with the natural alignment of their types, as a C
// struct without packing pragmas has (docs/gust/checked_and_externs.md).
#pragma once

namespace goose {

// A type's size and alignment in C, and its size as Goose lays it out. A
// layout that is not `known` is one this check does not model.
struct FfiLayout {
    int64_t csize = 0, calign = 1, gsize = 0;
    bool known = true;
};

inline int64_t FfiAlignUp(int64_t n, int64_t a) { return a > 1 ? (n + a - 1) / a * a : n; }

inline string FfiTypeStr(const TypeExpr *t) {
    string s;
    t->Dump(s);
    return s;
}

// `hazards` collects one line per difference, each naming the field by its
// path from the type that crosses (`Rect.b`).
inline FfiLayout FfiLayoutOf(TypeExpr *t, const string &path, vector<string> &hazards) {
    FfiLayout l;
    switch (t->kind) {
        case TY_INT:
            if (t->intstorage == IS_VARINT) { l.known = false; break; }
            l.csize = l.calign = l.gsize = IntBits(t->intstorage) / 8;
            break;
        case TY_FLT:
            l.csize = l.calign = l.gsize = t->fltstorage == FS_F32 ? 4 : 8;
            break;
        case TY_BOOL:
            l.csize = l.calign = l.gsize = 1;
            break;
        case TY_REF:
            if (t->ref->lenstorage >= 0) { l.known = false; break; }
            l.csize = l.calign = l.gsize = 8;
            break;
        case TY_ARRAY: {
            if (t->arr->akind != A_FIXED || t->arr->size < 0) { l.known = false; break; }
            auto e = FfiLayoutOf(t->arr->sub, cat(path, "[]"), hazards);
            if (!e.known) { l.known = false; break; }
            if (e.csize != e.gsize && t->arr->size > 1)
                hazards.push_back(cat(path, ": its elements are ", e.gsize, " bytes apart in Goose and ",
                                      e.csize, " in C"));
            l.csize = e.csize * t->arr->size;
            l.gsize = e.gsize * t->arr->size;
            l.calign = e.calign;
            break;
        }
        case TY_STRUCT: {
            auto st = t->struc->st;
            auto inst = t->struc->inst;
            int64_t goff = 0, coff = 0;
            for (size_t i = 0; i < st->fields.size(); i++) {
                auto &f = st->fields[i];
                if (f.ispad) {
                    if (f.padsize > 0) {
                        goff += f.padsize;
                        continue;
                    }
                    // A bare pad aligns the next field to its natural alignment.
                    for (size_t j = i + 1; j < st->fields.size(); j++) {
                        if (st->fields[j].ispad) continue;
                        auto ft = inst && j < inst->ftypes.size() && inst->ftypes[j] ? inst->ftypes[j]
                                                                                    : st->fields[j].type;
                        vector<string> ignored;
                        goff = FfiAlignUp(goff, FfiLayoutOf(ft, "", ignored).calign);
                        break;
                    }
                    continue;
                }
                auto ft = inst && i < inst->ftypes.size() && inst->ftypes[i] ? inst->ftypes[i] : f.type;
                auto fpath = cat(path, ".", f.name);
                auto fl = FfiLayoutOf(ft, fpath, hazards);
                if (!fl.known) return FfiLayout { 0, 1, 0, false };
                coff = FfiAlignUp(coff, fl.calign);
                if (goff != coff) {
                    auto gap = coff - goff;
                    hazards.push_back(gap > 0
                        ? cat(fpath, " is at offset ", goff, " in Goose and ", coff,
                              " in C: write `pad ", gap, "` before it")
                        : cat(fpath, " is at offset ", goff, " in Goose and ", coff,
                              " in C: a pad before it is ", -gap, " bytes too wide"));
                    goff = coff;   // Report each field's own difference, not every later one.
                }
                goff += fl.gsize;
                coff += fl.csize;
                l.calign = std::max(l.calign, fl.calign);
            }
            l.gsize = goff;
            l.csize = FfiAlignUp(coff, l.calign);
            if (l.csize != l.gsize)
                hazards.push_back(cat(path, " is ", l.gsize, " bytes in Goose and ", l.csize,
                                      " in C: write `pad ", l.csize - l.gsize,
                                      "` at its end where C keeps an array of them"));
            break;
        }
        default:
            l.known = false;
            break;
    }
    return l;
}

// The layout differences an extern fn's parameters and result could meet:
// a struct passed by value, by reference or as a slice's elements. Lines
// for types the check does not model say so instead.
inline vector<string> FfiLayoutHazards(SFunction *sf) {
    vector<string> out;
    auto check = [&](TypeExpr *t, const string &what) {
        if (!t) return;
        auto inner = t;
        if (t->kind == TY_REF && t->ref->lenstorage < 0) inner = t->ref->sub;
        else if (t->kind == TY_SLICE) inner = t->sub;
        if (inner->kind != TY_STRUCT && inner->kind != TY_ARRAY && inner->kind != TY_ENUM) return;
        if (inner->kind == TY_ENUM) {
            out.push_back(cat(what, ": a fixed-mode enum has no C layout this check models"));
            return;
        }
        vector<string> hazards;
        auto l = FfiLayoutOf(inner, FfiTypeStr(inner), hazards);
        if (!l.known) {
            out.push_back(cat(what, ": layout not checked (", FfiTypeStr(inner), ")"));
            return;
        }
        for (auto &h : hazards) out.push_back(cat(what, ": ", h));
        // By value, a `pad` is bytes the calling convention passes, where C's
        // own padding passes nothing: the C side has to declare those bytes
        // too. An extern block refuses this outright (DylibValueOk).
        if (inner == t && inner->kind == TY_STRUCT)
            for (auto &f : inner->struc->st->fields)
                if (f.ispad) {
                    out.push_back(cat(what, ": ", FfiTypeStr(inner),
                                      " passes by value with its pads, which the C side must "
                                      "declare as bytes too; or pass it by reference"));
                    break;
                }
    };
    for (size_t i = 0; i < sf->params.size(); i++)
        check(sf->params[i].type, cat("parameter ", sf->params[i].name));
    if (!sf->rets.empty()) check(sf->rets[0], "result");
    return out;
}

}  // namespace goose
