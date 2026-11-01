// Goose compiler — `goose build` (docs/design/packages.md §6): the C the
// compiler wrote, the runtime object and the native modules' response files,
// compiled and linked into an executable by the toolchain of toolchain.h.
// Intermediates live under GOOSE_HOME; the executable lands beside the root
// source unless -o says otherwise.
#pragma once

namespace goose {

// In main.cpp: the runtime's text hash, the runtime object's C, and a path's directory.
string RuntimeVersion();
string RuntimeObjectSource();
string DirOf(const string &path);

struct BuildOptions {
    bool debug = false;       // --debug: GS_DEBUG=1, debug information, no C optimization
    bool verbose = false;     // -v: print every command and what it prints
    bool dryrun = false;      // --dry-run: print the commands and run nothing
    bool standalone = false;  // --standalone: no runtime object
    string exe;               // -o: the executable; "" puts it beside the root source
    bool staticcrt = false;   // Gust: --static-crt, the C runtime linked in on Windows
};

inline string AbsolutePath(const string &path) {
    std::error_code ec;
    auto abs = filesystem::absolute(filesystem::path(path), ec);
    return ec ? path : abs.lexically_normal().string();
}

// GOOSE_HOME/build/prog/<stem>-<hash of the root's absolute path>/: where a
// program's C, objects and logs go.
inline string ProgramBuildDir(const string &rootfile) {
    auto abs = AbsolutePath(rootfile);
    #ifdef _WIN32
        for (auto &c : abs) c = (char)tolower((unsigned char)c);   // One directory per file, however spelled.
    #endif
    return cat(GooseHome(), "/build/prog/", PathStem(rootfile), "-", Hex8(abs));
}

// Runs the toolchain's commands, printing them as -v and --dry-run ask, and
// a failed one with what it printed.
struct Builder {
    const Toolchain &tc;
    const BuildOptions &opts;
    FILE *msgs;
    // Gust: the universal CRT as Windows' own DLL (UcrtDll), for a Windows
    // release build that links no C of a package's or its own.
    bool ucrtdll = false;

    // The compile flags of both styles: optimization, or debug information
    // and none of it. Defines go into the C itself (main.cpp), never here.
    // Gust: a release build puts each function and datum in a section of
    // its own, which lets the linker leave out what nothing reaches: most
    // of the runtime, for most programs (a quarter of compare/'s program).
    // The linkers of the msvc style do so by default without /DEBUG, the
    // others by LinkFlags.
    vector<string> CFlags() const {
        if (tc.style == "msvc") {
            vector<string> f = { "/nologo" };
            if (opts.debug) f.push_back("/Od"), f.push_back("/Zi");
            else f.push_back("/O2"), f.push_back("/Gy"), f.push_back("/Gw");
            return f;
        }
        if (opts.debug) return { "-O0", "-g" };
        vector<string> f = { "-O2" };
        if (tc.name == "clang" || tc.name == "gcc")
            f.push_back("-ffunction-sections"), f.push_back("-fdata-sections");
        // Gust: at -O2 gcc 12 to 14 vectorize only a loop whose count is known
        // to be a multiple of the vector's (the very-cheap model), which rules
        // out nearly all of them: to_lower over a slice ran 11 times slower
        // under Ubuntu's gcc 13 than under gcc 15. Older ones do not vectorize
        // at -O2 at all. This one leaves out loops needing checks at run time.
        if (tc.name == "gcc" && atoi(tc.version.c_str()) >= 8)
            f.push_back("-ftree-vectorize"), f.push_back("-fvect-cost-model=cheap");
        #ifdef __linux__
            // Gust: not what a distribution's compiler turns on by default
            // for C (Ubuntu's gcc all of it): stack protectors, CET's landing
            // pads, stack-clash probes and fortified copies guard against
            // what a checked language's own code does not do, as Rust's
            // default builds reckon; and unwind tables, which nothing here
            // unwinds. The flags are gcc 8's and clang 11's.
            if ((tc.name == "gcc" && atoi(tc.version.c_str()) >= 8) ||
                (tc.name == "clang" && atoi(tc.version.c_str()) >= 11))
                for (auto x : { "-fno-stack-protector", "-fcf-protection=none", "-fno-stack-clash-protection",
                                "-U_FORTIFY_SOURCE", "-fno-asynchronous-unwind-tables" })
                    f.push_back(x);
        #endif
        return f;
    }

    // Gust: the link flags for a release build, of both styles: the
    // universal CRT's import library in place of its static one (UcrtDll),
    // and in the cc style unreferenced sections left out (CFlags). Windows'
    // clang for the MSVC target links with the msvc-style linkers, which do
    // that already; MinGW's gcc and clang link with GNU's or lld's, which
    // are asked to.
    vector<string> LinkFlags() const {
        if (opts.debug) return {};
        if (tc.style == "msvc")
            return ucrtdll ? vector<string> { "/link", "/NODEFAULTLIB:libucrt.lib", "ucrt.lib" } : vector<string> {};
        if (tc.name != "clang" && tc.name != "gcc") return {};
        #if defined(__APPLE__)
            return { "-Wl,-dead_strip" };
        #elif defined(_WIN32)
            if (tc.name == "clang" && tc.target.find("windows-gnu") == string::npos)
                return ucrtdll ? vector<string> { "-Wl,/NODEFAULTLIB:libucrt.lib", "-lucrt" } : vector<string> {};
            return { "-Wl,--gc-sections", "-s" };
        #else
            // Without the symbol table, and (Linux) with the headers, read-only
            // data and code in fewer page-aligned segments.
            return { "-Wl,--gc-sections", "-s", "-Wl,-z,noseparate-code" };
        #endif
    }

    // Gust: whether a release build on Windows takes the universal CRT from
    // the DLL that Windows 10 and later ship (ucrtbase, through the
    // api-ms-win-crt-* names), linking the rest of the C runtime, vcruntime,
    // in: Microsoft's "hybrid CRT". That leaves the C library out of the
    // executable, compare/'s program going from 204 to 65 KB, without the
    // Visual C++ redistributable that vcruntime's DLL needs (Rust's msvc
    // target imports it), and its working set is a tenth of a megabyte
    // below that of both as DLLs. The objects are compiled as for the
    // static runtime, so the runtime object is that one. Only where nothing
    // else is linked: a package's C, and the C of the program's own sidecar,
    // may bring libraries of their own that ask for the static universal
    // CRT (native.h). For clang, the MSVC target only (a MinGW clang links
    // MinGW's own C runtime).
    bool UcrtDll(bool othercode) const {
        #ifdef _WIN32
            if (opts.debug || opts.staticcrt || othercode) return false;
            if (tc.style == "msvc") return true;
            return tc.name == "clang" && tc.target.find("windows-msvc") != string::npos;
        #else
            (void)othercode;
            return false;
        #endif
    }

    // The system libraries a program links beyond the C library, in the cc
    // style: none on Windows, where they live in the C runtime.
    vector<string> SystemLibs() const {
        #ifdef _WIN32
            return {};
        #else
            if (tc.style == "msvc") return {};
            vector<string> libs = { "-lm", "-pthread" };
            #ifdef __linux__
                // Gust: libm only where the program calls into it. The
                // runtime's own floats need none of it (gs_fmod, gs_floor,
                // gs_sqrt), and loading libm.so costs a third of a
                // megabyte resident; Ubuntu's gcc links so by default
                // already, clang does not.
                libs = { "-Wl,--as-needed", "-lm", "-Wl,--no-as-needed", "-pthread" };
                // dlopen, for the runtime's dynamic externs: in libdl before
                // glibc 2.34, and in libc since, where the archive left
                // behind is empty.
                libs.push_back("-ldl");
            #endif
            return libs;
        #endif
    }

    // Runs `argv`, `what` being what it does ("compiling the runtime"), with
    // its output kept in `logfile`. False when it failed, after saying so.
    bool Run(const vector<string> &argv, const string &what, const string &logfile) const {
        if (opts.dryrun || opts.verbose) fprintf(msgs, "%s\n", CommandLineText(argv).c_str());
        if (opts.dryrun) return true;
        fflush(msgs);
        ProcessResult r;
        {
            ProgressStep step(what);
            r = RunProcess(argv, "", opts.verbose);
        }
        if (!logfile.empty()) WriteFileMakingDirs(logfile, r.output);
        if (r.started && r.code == 0) return true;
        auto msg = cat(what, " failed", r.started ? cat(" (exit ", r.code, ")") : string(), ":\n    ",
                       CommandLineText(argv), "\n");
        if (!opts.verbose) msg += r.output;
        if (!logfile.empty())
            Append(msg, msg.back() == '\n' ? "" : "\n", "(the output is in ", logfile, ")");
        ProgressPrint(msg);
        return false;
    }

    // A system library by name, as the style's linker takes it.
    string LibFlag(const string &lib) const {
        return tc.style == "msvc" ? cat(lib, ".lib") : cat("-l", lib);
    }

    // Compiles the C file `src` to the object `obj`, with `extra` flags
    // (a package's includes and defines) after the usual ones.
    bool CompileObject(const string &src, const string &obj, const string &what,
                       const string &logfile, const vector<string> &extra = {}) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        for (auto &f : extra) argv.push_back(f);
        if (tc.style == "msvc") {
            argv.push_back("/c");
            argv.push_back(src);
            argv.push_back(cat("/Fo:", obj));
            if (opts.debug) argv.push_back(cat("/Fd:", obj, ".pdb"));
        } else {
            argv.push_back("-c");
            argv.push_back(src);
            argv.push_back("-o");
            argv.push_back(obj);
        }
        return Run(argv, what, logfile);
    }

    // Links `objects` and the system libraries `libs` (LibFlag spellings)
    // into the shared library `out`, with `builddir` taking the
    // intermediates: a package's JIT library.
    bool LinkShared(const vector<string> &objects, const vector<string> &libs, const string &out,
                    const string &builddir, const string &logfile, const string &what) const {
        vector<string> argv = { tc.cc };
        if (tc.style == "msvc") {
            argv.push_back("/nologo");
            argv.push_back("/LD");
            argv.push_back(cat("/Fe:", out));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            for (auto &o : objects) argv.push_back(o);
            for (auto &l : libs) argv.push_back(l);
        } else {
            argv.push_back("-shared");
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(out);
            for (auto &l : libs) argv.push_back(l);
            for (auto &l : SystemLibs()) argv.push_back(l);
        }
        return Run(argv, what, logfile);
    }

    // Compiles `src`, with `extra` flags after the usual ones, and links it
    // with `objects` and the response files `rsps` into `exe`, with
    // `builddir` taking the intermediates.
    bool Link(const string &src, const vector<string> &objects, const vector<string> &rsps,
              const string &exe, const string &builddir, const string &logfile,
              const vector<string> &extra = {}) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        for (auto &f : extra) argv.push_back(f);
        if (tc.style == "msvc") {
            argv.push_back(cat("/Fe:", exe));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            if (opts.debug) argv.push_back(cat("/Fd:", builddir, "\\", PathStem(exe), ".pdb"));
            argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            for (auto &f : LinkFlags()) argv.push_back(f);   // after /link, so last
        } else {
            argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(exe);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            for (auto &l : SystemLibs()) argv.push_back(l);
            for (auto &f : LinkFlags()) argv.push_back(f);
        }
        return Run(argv, cat("building ", exe), logfile);
    }
};

// The runtime object for this toolchain and configuration, compiled from
// `goose --emit-runtime`'s C on first use: GOOSE_HOME/build/runtime/<runtime
// hash>/<toolchain id>-<config>/goose_runtime.o. "" when it could not be
// built, after the builder said why.
inline string RuntimeObject(const Builder &b) {
    // Gust: a release object is built with sections per function (-gc) and,
    // where the C runtime is a DLL, against it (-md): each its own object.
    auto dir = cat(GooseHome(), "/build/runtime/", RuntimeVersion(), "/", b.tc.id, "-",
                   b.opts.debug ? "debug" : "release-gc");
    auto obj = cat(dir, "/goose_runtime.", b.tc.style == "msvc" ? "obj" : "o");
    if (PathExists(obj) && !b.opts.dryrun) return obj;
    auto src = cat(dir, "/goose_runtime.c");
    auto log = cat(dir, "/cc.log");
    if (b.opts.dryrun) {
        b.CompileObject(src, obj, "compiling the runtime", log);
        return obj;
    }
    std::error_code ec;
    filesystem::create_directories(filesystem::path(dir), ec);
    if (ec) throw CompileError { cat("cannot create ", dir, ": ", ec.message()) };
    // Every first build shares these paths. Wait before writing the source
    // or invoking the compiler, then recheck what the previous builder left.
    FileLock lock(cat(dir, "/build.lock"), "waiting for another goose compiling the runtime");
    if (PathExists(obj)) return obj;
    ProgressStep step(cat("compiling the runtime for ", b.tc.desc), true);
    if (!WriteFileMakingDirs(src, RuntimeObjectSource()))
        throw CompileError { cat("cannot write ", src) };
    // A failed or interrupted compiler may leave a partial object. Only a
    // successful compile publishes the name readers use, by atomic rename.
    auto tmp = cat(dir, "/goose_runtime.tmp.", b.tc.style == "msvc" ? "obj" : "o");
    filesystem::remove(filesystem::path(tmp), ec);
    if (ec) throw CompileError { cat("cannot remove ", tmp, ": ", ec.message()) };
    if (!b.CompileObject(src, tmp, "compiling the runtime", log)) return "";
    ec = RenamePath(tmp, obj);
    if (ec) throw CompileError { cat("cannot publish ", obj, ": ", ec.message()) };
    return obj;
}

// What `goose build` does once the compiler has written the program's C to
// `cfile` in `builddir`: finds the runtime object for the toolchain `tc`,
// which built the native parts whose response files are `nativersps`
// (native.h and builtin.h, found by the caller) in link order, a package's
// before the packages it requires and the program's own C first of all, and
// links the executable, named by -o, the program's sidecar (`output`) or the
// root file. The exit code.
// Gust: `simdunit` says the program's simd versions may be built in a unit
// of their own (main.cpp), which MSVC, with no per-function target, needs
// for their AVX2 bodies: the C compiled once more, for AVX2 and with
// GS_TU_AVX2, holding them alone, and the first unit with GS_TU_BASE, which
// links its dispatch to them (runtime.h). Not on another target than
// x86-64, nor --standalone, whose C holds the runtime's own data. The
// defines are the units' and not the program's, so they go on the command
// line, as no other define does.
inline int BuildExecutable(const string &rootfile, const string &cfile, const string &builddir,
                           const Toolchain &tc, const BuildOptions &opts, FILE *msgs,
                           const vector<string> &nativersps, const string &output,
                           bool simdunit = false) {
    Builder b { tc, opts, msgs };
    b.ucrtdll = b.UcrtDll(!nativersps.empty());
    vector<string> objects;
    if (!opts.standalone) {
        auto obj = RuntimeObject(b);
        if (obj.empty()) return 1;
        objects.push_back(obj);
    }
    vector<string> unitflags;
    auto ccdir = filesystem::path(tc.cc).parent_path().filename().string();
    for (auto &c : ccdir) c = (char)tolower((unsigned char)c);
    if (simdunit && tc.name == "msvc" && ccdir == "x64" && !opts.standalone) {
        auto obj = cat(builddir, "/", PathStem(cfile), "-avx2.obj");
        if (!b.CompileObject(cfile, obj, "compiling the AVX2 versions", cat(builddir, "/cc-avx2.log"),
                             { "/arch:AVX2", "/DGS_TU_AVX2" }))
            return 1;
        objects.push_back(obj);
        unitflags.push_back("/DGS_TU_BASE");
    }
    auto exe = opts.exe;
    if (exe.empty()) {
        exe = cat(DirOf(rootfile), output.empty() ? PathStem(rootfile) : output);
        #ifdef _WIN32
            if (exe.size() < 4 || exe.compare(exe.size() - 4, 4, ".exe") != 0) exe += ".exe";
        #endif
    }
    if (!opts.dryrun) {
        std::error_code ec;
        auto exedirpath = filesystem::path(exe).parent_path();
        if (!exedirpath.empty()) filesystem::create_directories(exedirpath, ec);
    }
    if (!b.Link(cfile, objects, nativersps, exe, builddir, cat(builddir, "/cc.log"), unitflags))
        return 1;
    if (!opts.dryrun) fprintf(msgs, "built %s\n", exe.c_str());
    return 0;
}

}  // namespace goose
