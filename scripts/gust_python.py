#!/usr/bin/env python3
"""Gust: Python bindings for a Gust library, as a package pip installs.

    python scripts/gust_python.py lib.gust [--name mylib] [--out dist] [--exe gust] [--cc clang]
    pip install dist/            (or: pip install --no-build-isolation dist/, offline)

The library's export fns (spec §7.11) become Python functions. The script
writes the library's C (`gust --standalone --header`), builds it as a
shared library with a C compiler, and writes `dist/mylib/__init__.py`, which
loads it with ctypes, starts the runtime (goose_init) as it is imported,
and wraps each export by the Goose signature the header gives it:

  Goose                     Python
  i8 .. i64, u8 .. u64      int, checked against the type's range
  f32, f64                  float
  bool                      bool
  struct S                  class S, a ctypes structure: a parameter takes
                            one, a tuple of its fields or a dict of them
  const T[:]                a sequence of T, or a buffer of T's machine
                            type (bytes, array.array, a numpy array), read
                            in place; a const u8[:] also takes a str (UTF-8)
  T[:]                      a list, which gets what the function wrote, or
                            a writable buffer (bytearray, array.array, a
                            numpy array), written in place
  -> const u8[:]            bytes, copied out at the call

A struct is bound when its fields are bound types and each sits at its
natural alignment, so that ctypes lays it out and passes it as C does
(Gust lays structs out packed). An export of any other type is left out, with a
comment in the module saying why. As for any C host, the functions run on
the thread that imported the module (another raises RuntimeError), and a
runtime error in Gust code (a failed bounds check, an abort) ends the
process with its message.
"""
import argparse, keyword, os, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Goose scalar: (ctypes type, size, kind, lo, hi); kind i/u integers, f float, b bool.
SCALARS = {"bool": ("c_bool", 1, "b", 0, 1), "f32": ("c_float", 4, "f", 0, 0), "f64": ("c_double", 8, "f", 0, 0)}
for _bits in (8, 16, 32, 64):
    SCALARS[f"i{_bits}"] = (f"c_int{_bits}", _bits // 8, "i", -2 ** (_bits - 1), 2 ** (_bits - 1) - 1)
    SCALARS[f"u{_bits}"] = (f"c_uint{_bits}", _bits // 8, "u", 0, 2 ** _bits - 1)
CSIZE = {"int8_t": 1, "uint8_t": 1, "int16_t": 2, "uint16_t": 2, "int32_t": 4, "uint32_t": 4, "int64_t": 8,
         "uint64_t": 8, "float": 4, "double": 8}
FN = re.compile(r"/\* gust: (\w+)\((.*)\)(?: -> (.+?))? \*/")
STRUCT = re.compile(r"/\* gust: struct (.+?) \{ (.*?) \} \*/\nstruct (\w+) \{\n(.*?)\n\};", re.S)


def pyname(n):
    n = re.sub(r"\W", "_", n)
    return n + "_" if keyword.iskeyword(n) else n


def split_fields(text):
    """[(name, type)] of "a: T, b: U", where a type may hold commas (a generic's)."""
    out, depth, cur = [], 0, ""
    for ch in text + ",":
        if ch == "," and depth == 0:
            if cur.strip():
                n, t = cur.split(":", 1)
                out.append((n.strip(), t.strip()))
            cur = ""
            continue
        depth += ch in "<([" and 1 or ch in ">)]" and -1 or 0
        cur += ch
    return out


class Types:
    """What the header says of the library's types, and how each becomes Python."""

    def __init__(self, header):
        self.structs = {}  # Goose name: (class, [(field, goose type)], [C fields], size, align) or a reason
        raw = {}
        for m in STRUCT.finditer(header):
            cfields = []
            for line in m.group(4).splitlines():
                f = re.fullmatch(r"\s*(\w+) (\w+)(?:\[(\d+)\])?;", line)
                cfields.append(f.groups() if f else (None, line.strip(), None))
            raw[m.group(1)] = (split_fields(m.group(2)), cfields)
        self.raw = raw
        self.order = []

    def struct(self, gname):
        """(class name, size, align) of a bound struct, or a str saying why it is not."""
        if gname in self.structs:
            return self.structs[gname]
        if gname not in self.raw:
            return f"{gname} is not a struct the header lays out"
        self.structs[gname] = f"{gname} contains itself"
        fields, cfields = self.raw[gname]
        off, align, layout, gi = 0, 1, [], 0
        for ctype, cname, count in cfields:
            if ctype is None:
                return self._no(gname, f"{gname} has a field C declares as {cname}")
            if cname.startswith("gs_pad"):
                layout.append((f"_pad{len(layout)}", f"_c.c_uint8 * {count}", None))
                off += int(count)
                continue
            if cname == "gs_empty" or gi >= len(fields) or count:
                return self._no(gname, f"{gname} has a field Python has no form for")
            fname, ftype = fields[gi]
            gi += 1
            if ftype in SCALARS:
                ct, size, _, _, _ = SCALARS[ftype]
                ct, a = f"_c.{ct}", size
            else:
                sub = self.struct(ftype)
                if isinstance(sub, str):
                    return self._no(gname, f"its field {fname} has type {ftype}: {sub}")
                ct, size, a = sub
            if off % a:
                return self._no(gname, f"its field {fname} is at offset {off}, not at a multiple of {a}")
            layout.append((pyname(fname), ct, ftype))
            off += size
            align = max(align, a)
        if gi != len(fields) or off % align:
            return self._no(gname, f"{gname} ends at {off}, not at a multiple of {align}")
        cls = pyname(gname)
        self.structs[gname] = (cls, off, align)
        self.order.append((gname, cls, layout))
        return self.structs[gname]

    def _no(self, gname, why):
        self.structs[gname] = why
        return why

    def conv(self, gtype):
        """(the Python expression of a converter (where, value) -> C value, None) for a Goose
        type, or (None, why it has none)."""
        if gtype in SCALARS:
            return f"_{gtype}", None
        s = self.struct(gtype)
        return (None, s) if isinstance(s, str) else (f"{s[0]}._from", None)

    def ctype(self, gtype):
        return f"_c.{SCALARS[gtype][0]}" if gtype in SCALARS else self.struct(gtype)[0]


PRELUDE = '''"""Python bindings for the Gust library NAME, generated by gust_python.py: do not edit."""
import ctypes as _c
import operator as _operator
import os as _os
import sys as _sys
import threading as _threading

_ext = {"win32": ".dll", "darwin": ".dylib"}.get(_sys.platform, ".so")
_lib = _c.CDLL(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "_NAME" + _ext))


def _ints(lo, hi):
    def conv(where, x):
        try:
            x = _operator.index(x)
        except TypeError:
            raise TypeError(f"{where}: an integer, not {type(x).__name__}") from None
        if not lo <= x <= hi:
            raise OverflowError(f"{where}: {x} is outside [{lo}, {hi}]")
        return x
    return conv


def _f64(where, x):
    if isinstance(x, (str, bytes, bytearray)):
        raise TypeError(f"{where}: a number, not {type(x).__name__}")
    return float(x)


def _bool(where, x):
    return bool(x)


_f32 = _f64
SCALARS


class _Struct(_c.Structure):
    """A Gust struct: built from one of its kind, a tuple of its fields or a dict of them."""

    def __iter__(self):
        return (getattr(self, k) for k in self._names)

    def __eq__(self, other):
        return type(other) is type(self) and tuple(self) == tuple(other)

    def __repr__(self):
        return type(self).__name__ + "(" + ", ".join(f"{k}={getattr(self, k)!r}" for k in self._names) + ")"

    @classmethod
    def _from(cls, where, x):
        if isinstance(x, cls):
            return x
        if isinstance(x, dict):
            if set(x) != set(cls._names):
                raise TypeError(f"{where}: a {cls.__name__} has the fields {', '.join(cls._names)}")
            x = [x[k] for k in cls._names]
        x = tuple(x)
        if len(x) != len(cls._names):
            raise TypeError(f"{where}: a {cls.__name__} has {len(cls._names)} fields, not {len(x)}")
        out = cls()
        for k, conv, v in zip(cls._names, cls._convs, x):
            setattr(out, k, conv(where + "." + k, v))
        return out


_slices = {}


def _slice_type(ctype):
    """The C struct of a slice of ctype: a pointer to its first element and its length."""
    if ctype not in _slices:
        class Slice(_c.Structure):
            _fields_ = [("data", _c.POINTER(ctype)), ("len", _c.c_int64)]
        _slices[ctype] = Slice
    return _slices[ctype]


_KIND = dict.fromkeys("bhilqn", "i")
_KIND.update(dict.fromkeys("BHILQN", "u"))
_KIND.update({"f": "f", "d": "f", "?": "b"})
_NATIVE = {"little": "<", "big": ">"}[_sys.byteorder]


def _buffer(where, x, ctype, kind, writable):
    """x's memory as an array of ctype when x is a buffer of that machine type, else None."""
    try:
        m = memoryview(x)
    except TypeError:
        return None
    f = m.format[1:] if m.format[:1] in ("@", "=", _NATIVE) else m.format
    if m.ndim != 1 or not m.c_contiguous or _KIND.get(f) != kind or m.itemsize != _c.sizeof(ctype):
        if writable:
            raise TypeError(f"{where}: a buffer of {m.format} elements, where {ctype.__name__} ones are wanted")
        return None
    if m.readonly:
        if writable:
            raise TypeError(f"{where}: the function may write to it, and this buffer is read only")
        return (ctype * len(m)).from_buffer_copy(m)
    return (ctype * len(m)).from_buffer(m)


def _slice_in(where, x, ctype, kind, conv, writable):
    """The C slice of x, the array it points to, and a list to copy the array back to."""
    if isinstance(x, str) and ctype is _c.c_uint8 and not writable:
        x = x.encode()
    arr = None if kind == "s" else _buffer(where, x, ctype, kind, writable)
    back = None
    if arr is None:
        if writable and not isinstance(x, list):
            raise TypeError(f"{where}: the function may write to it, so it takes a list or a writable buffer")
        arr = (ctype * len(x))(*[conv(f"{where}[{i}]", v) for i, v in enumerate(x)])
        back = x if writable else None
    # A pointer past nothing for an empty slice: the length says not to read it.
    ptr = _c.cast(arr if len(arr) else (ctype * 1)(), _c.POINTER(ctype))
    return _slice_type(ctype)(ptr, len(arr)), arr, back


_argv = (_c.c_char_p * 2)(b"NAME", None)
_lib.goose_init.argtypes = [_c.c_int, _c.POINTER(_c.c_char_p)]
_lib.goose_init.restype = None
_lib.goose_init(1, _argv)
_owner = _threading.get_ident()


def _on_owner(name):
    if _threading.get_ident() != _owner:
        raise RuntimeError(f"{name}: NAME's Gust functions run on the thread that imported it")
'''


def module_text(name, header):
    """The package's __init__.py, and the names it binds."""
    types = Types(header)
    exports = []
    for m in FN.finditer(header):
        exports.append((m.group(1), split_fields(m.group(2)), m.group(3)))
    body, names = [], []
    for fname, params, ret in exports:
        why = None
        argtypes, conv, after, call = [], [], [], []
        pnames = [pyname(p) for p, _ in params]
        for (pname, ptype), py in zip(params, pnames):
            where = f"{fname}({pname})"
            se = re.fullmatch(r"(const )?(.+)\[:\]", ptype)
            if se:
                et = se.group(2)
                c, no = types.conv(et)
                if no:
                    why = why or f"parameter {pname} is a slice of {et}, and {no}"
                    continue
                kind = SCALARS[et][2] if et in SCALARS else "s"
                ct = types.ctype(et)
                argtypes.append(f"_slice_type({ct})")
                conv.append(f'    _a_{py}, _k_{py}, _b_{py} = _slice_in("{where}", {py}, {ct}, "{kind}", {c}, '
                            f'{not se.group(1)})')
                if not se.group(1):
                    after += [f"    if _b_{py} is not None:", f"        _b_{py}[:] = list(_k_{py})"]
            else:
                c, no = types.conv(ptype)
                if no:
                    why = why or f"parameter {pname} has type {ptype}: {no}"
                    continue
                argtypes.append(types.ctype(ptype))
                conv.append(f'    _a_{py} = {c}("{where}", {py})')
            call.append(f"_a_{py}")
        restype, outbytes = "None", False
        if ret == "const u8[:]":
            restype, outbytes = "_c.POINTER(_c.c_uint8)", True
            argtypes.append("_c.POINTER(_c.c_int64)")
            call.append("_c.byref(_n)")
        elif ret:
            c, no = types.conv(ret)
            if no:
                why = why or f"it returns {ret}: {no}"
            else:
                restype = types.ctype(ret)
        sig = f"{fname}({', '.join(f'{p}: {t}' for p, t in params)})" + (f" -> {ret}" if ret else "")
        if why:
            body += ["", f"# {sig}: not bound, since {why}.", ""]
            continue
        py = pyname(fname)
        names.append(py)
        fn = f"_fn_{py}"
        body += ["", f'{fn} = _lib["{fname}"]', f"{fn}.argtypes = [{', '.join(argtypes)}]", f"{fn}.restype = {restype}",
                 "", "", f"def {py}({', '.join(pnames)}):", f'    """{sig}"""', f'    _on_owner("{fname}")'] + conv
        if outbytes:
            body += ["    _n = _c.c_int64(0)", f"    _r = _c.string_at({fn}({', '.join(call)}), _n.value)"]
        else:
            body.append(f"    _r = {fn}({', '.join(call)})")
        body += after + ["    return _r", ""]
    # The structs, each after those it holds.
    classes = []
    for gname, cls, layout in types.order:
        fields = [f for f in layout if f[2] is not None]
        classes += ["", "", f"class {cls}(_Struct):", f'    """struct {gname} in Gust"""',
                    "    _fields_ = [" + ", ".join(f'("{n}", {ct})' for n, ct, _ in layout) + "]",
                    f"    _names = ({''.join(repr(n) + ', ' for n, _, _ in fields)})",
                    f"{cls}._convs = ({''.join(types.conv(t)[0] + ', ' for _, _, t in fields)})"]
        names.append(cls)
    for gname, why in types.structs.items():
        if isinstance(why, str):
            classes += ["", f"# struct {gname}: not bound, since {why}."]
    scalars = "\n".join(f"_{g} = _ints({lo}, {hi})" for g, (_, _, k, lo, hi) in SCALARS.items() if k in "iu")
    text = PRELUDE.replace("NAME", name).replace("SCALARS", scalars)
    text += "\n".join(classes) + "\n" + "\n".join(body) + f"\n\n__all__ = {names!r}\n"
    return text, names


# The package holds a native library, so its wheel is for this platform,
# though for any Python 3: ctypes loads it, not the interpreter's C API.
SETUP = '''from setuptools import setup

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:
    from wheel.bdist_wheel import bdist_wheel


class PlatformWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        return ("py3", "none", super().get_tag()[2])


setup(cmdclass={"bdist_wheel": PlatformWheel})
'''


def find_cc(given):
    if given:
        return given
    # clang first, as the toolchain picks, then the system's own.
    p = shutil.which("clang")
    if p:
        return p
    p = Path(r"C:\Program Files\LLVM\bin\clang.exe")
    if p.exists():
        return str(p)
    for c in ("cc", "gcc"):
        p = shutil.which(c)
        if p:
            return p
    sys.exit("no C compiler found: give one with --cc")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--name", help="the Python package's name (default: the source's stem)")
    ap.add_argument("--out", default="dist")
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--exe", default=str(ROOT / "build" / ("Release/gust.exe" if os.name == "nt" else "gust")))
    ap.add_argument("--cc")
    args = ap.parse_args()
    src = Path(args.source).resolve()
    name = args.name or src.stem
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or keyword.iskeyword(name):
        sys.exit(f"{name} is not a Python package name: give one with --name")
    out = Path(args.out).resolve()
    pkg = out / name
    pkg.mkdir(parents=True, exist_ok=True)
    # The C beside the package, apart from the build/ setuptools uses.
    work = out / "c"
    work.mkdir(exist_ok=True)
    cfile, hfile = work / f"{name}.c", work / f"{name}.h"
    r = subprocess.run([args.exe, "-O2", "--standalone", "--header", str(hfile), "-q", "-o", str(cfile), str(src)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"gust: {r.stdout}{r.stderr}")
    header = hfile.read_text(encoding="utf-8")
    text, names = module_text(name, header)
    cc = find_cc(args.cc)
    ext = {"win32": ".dll", "darwin": ".dylib"}.get(sys.platform, ".so")
    lib = pkg / f"_{name}{ext}"
    deffile = work / f"{name}.def"
    deffile.write_text("EXPORTS\n    goose_init\n" + "".join(f"    {m.group(1)}\n" for m in FN.finditer(header)))
    if Path(cc).stem.lower() in ("cl", "clang-cl"):
        # A DLL exports what a .def file names: goose_init and the exports.
        cmd = [cc, "/nologo", "/O2", "/w", "/LD", str(cfile), f"/Fe:{lib}", f"/Fo:{work}{os.sep}", "/link",
               f"/DEF:{deffile}", f"/IMPLIB:{work / name}.lib"]
    else:
        cmd = [cc, "-O2", "-shared", "-w", "-o", str(lib), str(cfile)]
        if os.name == "nt":
            target = subprocess.run([cc, "-dumpmachine"], capture_output=True, text=True).stdout
            # MSVC's linker takes the .def as an option, and its import
            # library goes beside the C; MinGW's ld takes the .def as an input.
            if "msvc" in target:
                cmd += [f"-Wl,/DEF:{deffile}", f"-Wl,/IMPLIB:{work / name}.lib"]
            else:
                cmd.append(str(deffile))
        else:
            # The runtime's threads, libm, and dlopen for dynamic externs
            # (in libdl on Linux before glibc 2.34).
            cmd[1:1] = ["-fPIC"]
            cmd += ["-pthread", "-lm"] + (["-ldl"] if sys.platform.startswith("linux") else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{cc}: {r.stdout}{r.stderr}")
    (pkg / "__init__.py").write_text(text, encoding="utf-8", newline="\n")
    (out / "pyproject.toml").write_text(
        '[build-system]\nrequires = ["setuptools>=61"]\nbuild-backend = "setuptools.build_meta"\n\n'
        f'[project]\nname = "{name}"\nversion = "{args.version}"\n'
        f'description = "Python bindings for the Gust library {name}"\nrequires-python = ">=3.8"\n\n'
        f'[tool.setuptools]\npackages = ["{name}"]\n\n[tool.setuptools.package-data]\n'
        f'{name} = ["_{name}.dll", "_{name}.so", "_{name}.dylib"]\n', encoding="utf-8", newline="\n")
    (out / "setup.py").write_text(SETUP, encoding="utf-8", newline="\n")
    print(f"wrote {pkg}: {', '.join(names) or 'nothing'} bound; pip install {out}")


if __name__ == "__main__":
    main()
