// Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
// Goose compiler — packages (docs/design/packages.md §4): what a sourced
// import, `import name from "source";`, names, and where that package's
// files are. A path source is a directory used in place, relative to the
// file that wrote it; a git source is fetched into GOOSE_HOME (§4.6).
#pragma once

namespace goose {

// In main.cpp.
string RelativeTo(const string &path, const string &dir);

struct PackageSource {
    string text;        // As written.
    bool ispath = false;
    string dir;         // A path source: the package's root, absolute, forward slashes, trailing /.
    string url, ref;    // A git source: what git clones, and the tag, branch or commit.
};

inline string ForwardSlashes(string path) {
    for (auto &c : path) if (c == '\\') c = '/';
    return path;
}

// `dir` absolute, with forward slashes and one trailing slash.
inline string PackageDir(const string &dir) {
    auto abs = ForwardSlashes(AbsolutePath(dir));
    while (abs.size() > 1 && abs.back() == '/') abs.pop_back();
    return abs + "/";
}

inline bool IsAbsoluteSource(const string &text) {
    return (!text.empty() && (text[0] == '/' || text[0] == '\\')) ||
           (text.size() > 1 && text[1] == ':');
}

inline bool IsPathSource(const string &text) {
    return text == "." || text == ".." || text.rfind("./", 0) == 0 || text.rfind("../", 0) == 0 ||
           text.rfind(".\\", 0) == 0 || text.rfind("..\\", 0) == 0 || IsAbsoluteSource(text);
}

// Reads a source as written in a file in `fromdir`. A path is resolved from
// there; a git source is `host/owner/repo@ref`, `owner/repo@ref` for
// GitHub, or any URL git takes with `@ref` behind it. Throws for a shape
// that is neither.
inline PackageSource ParsePackageSource(const string &text, const string &fromdir) {
    PackageSource s;
    s.text = text;
    if (text.empty()) throw CompileError { "a package source is a path or a git repository" };
    if (IsPathSource(text)) {
        s.ispath = true;
        s.dir = PackageDir(IsAbsoluteSource(text) ? text : cat(fromdir, text));
        return s;
    }
    auto at = text.rfind('@');
    // A `user@host:` prefix is part of the URL, not the ref.
    if (at == string::npos || at == 0 || text.find(':', at) != string::npos)
        throw CompileError { cat("the git source ", text, " needs a ref: host/owner/repo@tag, "
                                 "@branch or @commit") };
    s.ref = text.substr(at + 1);
    auto repo = text.substr(0, at);
    if (repo.find("://") != string::npos || repo.find('@') != string::npos) {
        s.url = repo;
    } else {
        auto slashes = (int)std::count(repo.begin(), repo.end(), '/');
        if (slashes == 1) s.url = cat("https://github.com/", repo);
        else if (slashes >= 2) s.url = cat("https://", repo);
        else throw CompileError { cat("the git source ", text, " is not owner/repo@ref, "
                                      "host/owner/repo@ref or a URL") };
    }
    if (s.ref.empty() || s.url.empty())
        throw CompileError { cat("the git source ", text, " needs a repository and a ref") };
    return s;
}

// Whether two sources name the same place: the same directory, or the same
// repository (refs aside).
inline bool SameSource(const PackageSource &a, const PackageSource &b) {
    if (a.ispath != b.ispath) return false;
    return a.ispath ? a.dir == b.dir : a.url == b.url;
}

// --- fetching (§4.6) ------------------------------------------------------------

// `goose fetch --update`: refs that are not commits are fetched again.
inline bool package_update = false;
// `--offline`: nothing is fetched; a package whose checkout is not in the
// cache is an error at its import, which `goose fetch` resolves.
inline bool package_nofetch = false;

inline bool IsCommitHash(const string &ref) {
    if (ref.size() < 7 || ref.size() > 40) return false;
    for (auto c : ref) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

// Where a git source is checked out: GOOSE_HOME/pkg/<host>/<owner>/<repo>/
// <ref> for the usual repository URL, and for any other URL (a file:// one,
// a deeper path) GOOSE_HOME/pkg/<repo>-<hash of the URL>/<ref>, which keeps
// the path short where Windows limits it. A commit is named by its first
// twelve digits, a branch's slashes become underscores.
inline string GitCacheKey(const PackageSource &s) {
    auto key = s.url;
    if (auto p = key.find("://"); p != string::npos) key = key.substr(p + 3);
    while (!key.empty() && key.back() == '/') key.pop_back();
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".git") == 0) key.resize(key.size() - 4);
    auto safe = true;
    for (auto c : key)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '/')) safe = false;
    if (!safe || std::count(key.begin(), key.end(), '/') != 2 || key[0] == '/') {
        auto repo = key.substr(key.find_last_of('/') + 1);
        for (auto &c : repo) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) c = '_';
        key = cat(repo.empty() ? "repo" : repo, "-", Hex8(s.url));
    }
    auto ref = IsCommitHash(s.ref) ? s.ref.substr(0, 12) : s.ref;
    for (auto &c : ref) if (c == '/' || c == '\\') c = '_';
    // Gust: a version range's characters that no Windows path takes.
    for (auto &c : ref) if (strchr("<>:\"|?*", c)) c = '_';
    return cat(key, "/", ref);
}

// Gust: the cache key of a package's checkout, where its sources are: the
// ref it was fetched at, not the range or lock entry that chose it.
inline string GitCacheKeyOf(const PackageSource &s, const string &root) {
    auto prefix = cat(GooseHome(), "/pkg/");
    if (root.rfind(prefix, 0) == 0 && root.size() > prefix.size())
        return root.substr(prefix.size(), root.size() - prefix.size() - (root.back() == '/' ? 1 : 0));
    return GitCacheKey(s);
}

inline string GitCacheDir(const PackageSource &s) {
    return cat(GooseHome(), "/pkg/", GitCacheKey(s));
}

// Reads the package's sidecar, `<name>.build` beside its entry, if there is
// one (§4.7): the C it compiles, where to find headers, what to define and
// link, or the CMake target to build; the directories that are none of its
// own; and for the program its executable's name. Lists take a _windows,
// _linux or _macos suffix for a platform's own part. Paths are relative to
// the package root.
inline void LoadSidecar(Package &pkg, bool isprogram) {
    pkg.sidecar = cat(pkg.root, pkg.name, ".build");
    auto bf = LoadBuildFile(pkg.sidecar, pkg.hassidecar);
    if (!pkg.hassidecar) return;
    const char *lists[] = { "c_sources", "c_include", "c_headers", "c_defines", "c_flags_cc",
                            "c_flags_msvc", "c_libs", "dylibs" };
    vector<string> known = { "cmake_target", "cmake_options", "main_thread_only", "exclude", "edition" };
    for (auto key : lists)
        for (auto suffix : { "", "_windows", "_linux", "_macos" }) known.push_back(cat(key, suffix));
    if (isprogram) known.push_back("output");
    bf.CheckKeys(known);
    #ifdef _WIN32
        const char *platform = "_windows";
    #elif defined(__APPLE__)
        const char *platform = "_macos";
    #else
        const char *platform = "_linux";
    #endif
    auto list = [&](const char *key, bool paths) {
        auto v = bf.Strings(key);
        for (auto &e : bf.Strings(cat(key, platform))) v.push_back(e);
        if (paths)
            for (auto &e : v)
                if (!IsAbsoluteSource(e)) e = ForwardSlashes(AbsolutePath(cat(pkg.root, e)));
        return v;
    };
    pkg.csources = list("c_sources", true);
    pkg.cincludes = list("c_include", true);
    // A `<name>` entry is a system header, left for the include path to find.
    pkg.cheaders = list("c_headers", false);
    for (auto &h : pkg.cheaders)
        if (!h.empty() && h[0] != '<' && !IsAbsoluteSource(h))
            h = ForwardSlashes(AbsolutePath(cat(pkg.root, h)));
    pkg.cdefines = list("c_defines", false);
    pkg.cflags_cc = list("c_flags_cc", false);
    pkg.cflags_msvc = list("c_flags_msvc", false);
    pkg.clibs = list("c_libs", false);
    pkg.cmake_target = bf.Str("cmake_target");
    pkg.cmake_options = bf.Strings("cmake_options");
    pkg.main_thread_only = bf.Bool("main_thread_only");
    // Gust: the edition the source is written for (docs/gust/stability.md),
    // of which there is one so far.
    if (auto ed = bf.Str("edition"); !ed.empty() && ed != "2026")
        throw CompileError { cat(pkg.sidecar, ": edition \"", ed, "\" is not one this compiler knows: ",
                                 "it knows \"2026\"") };
    for (auto &e : bf.Strings("exclude")) pkg.exclude.push_back(PackageDir(cat(pkg.root, e)));
    if (isprogram) pkg.output = bf.Str("output");
    // dylibs maps a name the source's extern blocks use to the spelling
    // to open on every platform, and the suffixed lists on one; the
    // generated C carries all of them, the runtime picking its own. A name
    // no block declares is refused once every block is known (ParseProgram).
    for (auto suffix : { "", "_windows", "_linux", "_macos" }) {
        auto key = cat("dylibs", suffix);
        for (auto &e : bf.Strings(key)) {
            auto eq = e.find('=');
            if (eq == string::npos || eq == 0 || eq + 1 == e.size())
                throw CompileError { cat(pkg.sidecar, ": ", key, " entry \"", e,
                                         "\" is written name=spelling") };
            auto name = e.substr(0, eq), spelling = e.substr(eq + 1);
            auto it = std::find_if(pkg.dylibs.begin(), pkg.dylibs.end(),
                                   [&](auto &d) { return d.name == name; });
            if (it == pkg.dylibs.end()) it = pkg.dylibs.insert(it, DylibSpelling { name });
            auto &slot = *suffix ? it->On(suffix) : it->any;
            if (!slot.empty())
                throw CompileError { cat(pkg.sidecar, ": ", key, " maps ", name, " twice") };
            slot = spelling;
        }
    }
    if (!pkg.csources.empty() && !pkg.cmake_target.empty())
        throw CompileError { cat(pkg.sidecar, ": a package has either c_sources or a cmake_target") };
}

// Removes a directory tree. A git checkout holds read-only files, which
// Windows will not delete until they are writable.
inline void RemoveTree(const string &path) {
    std::error_code ec;
    filesystem::path p(path);
    if (!filesystem::exists(p, ec)) return;
    #ifdef _WIN32
        for (filesystem::recursive_directory_iterator it(p, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code pec;
            filesystem::permissions(it->path(), filesystem::perms::owner_write,
                                    filesystem::perm_options::add, pec);
        }
    #endif
    filesystem::remove_all(p, ec);
}

// What a terminal shows of `output`, where a carriage return alone starts
// the line over: the last of each line's redraws, as git prints progress.
inline string AsShown(const string &output) {
    string shown;
    for (size_t i = 0; i < output.size();) {
        auto end = output.find('\n', i);
        if (end == string::npos) end = output.size();
        auto line = output.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Append(shown, line.substr(line.find_last_of('\r') + 1), end < output.size() ? "\n" : "");
        i = end + 1;
    }
    return shown;
}

// Runs git with `args`, and throws what it printed when it failed. A
// checkout in the cache can lie deep, past the path length Windows allows
// git by default. A checkout at a tag is detached by design, which git
// would otherwise explain even with -q.
inline void RunGit(const string &git, const vector<string> &args) {
    vector<string> argv = { git, "-c", "core.longpaths=true", "-c", "advice.detachedHead=false" };
    for (auto &a : args) argv.push_back(a);
    auto r = RunProcess(argv);
    if (r.started && r.code == 0) return;
    throw CompileError { cat(CommandLineText(argv), "\n", AsShown(r.output)) };
}

// The checkout of the git source `s` for the package `name`, fetched into
// the cache if it is not there (or, for a tag or branch, when --update asks
// for it again): a shallow clone at the ref, with its submodules, or a
// fetch of the one commit. An interrupted fetch leaves a .tmp directory,
// never a half checkout under the name. A lock beside it keeps concurrent
// fetches from removing or writing each other's temporary checkout.
inline string FetchPackage(const PackageSource &s, const string &name) {
    auto dir = GitCacheDir(s);
    std::error_code ec;
    auto have = filesystem::is_directory(filesystem::path(dir), ec);
    if (!have && package_nofetch)
        throw CompileError { cat("the package ", name, " from ", s.text, " is not fetched, and "
                                 "--offline fetches nothing: `goose fetch` fetches it") };
    auto update = have && package_update && !IsCommitHash(s.ref) && !package_nofetch;
    if (!have || update) {
        auto parent = filesystem::path(dir).parent_path();
        filesystem::create_directories(parent, ec);
        if (ec) throw CompileError { cat("cannot create ", parent.string(), ": ", ec.message()) };
        // The lock stays outside both the temporary and final checkout, so
        // cleanup and publication never remove a lock another process holds.
        FileLock lock(dir + ".lock", cat("waiting for another goose fetching the package ", name));
        have = filesystem::is_directory(filesystem::path(dir), ec);
        update = have && package_update && !IsCommitHash(s.ref);
        if (have && !update) return dir + "/";
        auto git = FindOnPath("git");
        if (git.empty())
            throw CompileError { cat("cannot find `git` on PATH, needed to fetch the package ", name,
                                     " from ", s.text, ". Install git, or clone that repository "
                                     "yourself and declare it as\n    import ", name,
                                     " from \"<the directory>\";") };
        ProgressStep step(cat(update ? "updating" : "fetching", " the package ", name, " from ", s.text),
                          true);
        // Quiet, but for the progress a live line can show.
        auto progress = ProgressLive() ? "--progress" : "--no-progress";
        try {
            if (update) {
                RunGit(git, { "-C", dir, "fetch", "-q", progress, "--depth", "1", s.url, s.ref });
                RunGit(git, { "-C", dir, "checkout", "-q", "--detach", "FETCH_HEAD" });
                RunGit(git, { "-C", dir, "submodule", "update", "-q", "--init", "--recursive",
                              "--depth", "1" });
            } else {
                auto tmp = dir + ".tmp";
                RemoveTree(tmp);
                try {
                    if (IsCommitHash(s.ref)) {
                        RunGit(git, { "init", "-q", tmp });
                        RunGit(git, { "-C", tmp, "fetch", "-q", progress, "--depth", "1", s.url,
                                      s.ref });
                        RunGit(git, { "-C", tmp, "checkout", "-q", "--detach", "FETCH_HEAD" });
                        RunGit(git, { "-C", tmp, "submodule", "update", "-q", "--init",
                                      "--recursive", "--depth", "1" });
                    } else {
                        RunGit(git, { "clone", "-q", progress, "--depth", "1", "--branch", s.ref,
                                      "--recurse-submodules", "--shallow-submodules", s.url, tmp });
                    }
                } catch (CompileError &) {
                    RemoveTree(tmp);
                    throw;
                }
                ec = RenamePath(tmp, dir);
                if (ec)
                    throw CompileError { cat("cannot move ", tmp, " to ", dir, ": ", ec.message()) };
            }
        } catch (CompileError &e) {
            throw CompileError { cat("cannot fetch the package ", name, " from ", s.text, ":\n",
                                     e.msg) };
        }
    }
    return dir + "/";
}

// --- versions and the lockfile (Gust) ------------------------------------------------
//
// A git source's ref may be a version range, `owner/repo@^1.2`, which the
// highest tag of the repository it allows satisfies: `^1.2.3` takes 1.x.y
// from 1.2.3 on (0.2.x from 0.2.3 for a 0 major), `~1.2.3` takes 1.2.y from
// 1.2.3, `>=1.2` any from 1.2.0, and `1.x` or `1.2.x` what the x leaves
// open. Tags are read as versions with or without a `v` in front.
//
// The program's lockfile, `<root>.lock` beside its root file, holds for
// each package from git the ref it was fetched at and the commit that
// was: a range resolves once, and a tag or branch that moves in the shared
// cache does not move the program, until `gust fetch --update` resolves
// them all again and rewrites the file.

struct Version { int64_t major = -1, minor = 0, patch = 0; };

// The version a tag names, if it names one: `1.2.3`, `v1.2`.
inline Version TagVersion(const string &tag) {
    Version v;
    auto s = tag;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    int64_t parts[3] = { 0, 0, 0 };
    size_t i = 0;
    int n = 0;
    for (; n < 3 && i < s.size(); n++) {
        if (!isdigit((unsigned char)s[i])) return {};
        int64_t x = 0;
        while (i < s.size() && isdigit((unsigned char)s[i])) x = x * 10 + (s[i++] - '0');
        parts[n] = x;
        if (i < s.size()) {
            if (s[i] != '.' || n == 2) return {};
            i++;
        }
    }
    if (n == 0 || i != s.size()) return {};
    return { parts[0], parts[1], parts[2] };
}

inline bool Before(const Version &a, const Version &b) {
    if (a.major != b.major) return a.major < b.major;
    if (a.minor != b.minor) return a.minor < b.minor;
    return a.patch < b.patch;
}

inline bool IsVersionRange(const string &ref) {
    if (ref.empty()) return false;
    if (ref[0] == '^' || ref[0] == '~' || ref.rfind(">=", 0) == 0) return true;
    return ref.find(".x") != string::npos || ref.find(".*") != string::npos;
}

// Whether version v is in `range`; throws for a range of no known shape.
inline bool InRange(const Version &v, const string &range) {
    auto bad = [&] {
        return CompileError { cat("the version range ", range, " is not ^1.2.3, ~1.2.3, >=1.2.3, "
                                  "1.x or 1.2.x") };
    };
    if (range.find(".x") != string::npos || range.find(".*") != string::npos) {
        auto parts = range;
        auto star = parts.find_first_of("x*");
        auto fixed = TagVersion(parts.substr(0, star - 1));
        if (fixed.major < 0) throw bad();
        auto dots = std::count(parts.begin(), parts.begin() + star, '.');
        return v.major == fixed.major && (dots < 2 || v.minor == fixed.minor);
    }
    auto at = range[0] == '>' ? 2 : 1;
    auto low = TagVersion(range.substr(at));
    if (low.major < 0) throw bad();
    if (Before(v, low)) return false;
    if (range[0] == '>') return true;
    if (range[0] == '~') return v.major == low.major && v.minor == low.minor;
    // ^: the same major, and for 0.y the same minor.
    if (low.major > 0) return v.major == low.major;
    return v.major == 0 && v.minor == low.minor;
}

struct LockEntry { string source, ref, commit; };
inline map<string, LockEntry> package_lock;   // By package name.
inline string package_lockfile;               // Where it is read from and written to.
inline bool package_lockdirty = false;

// Reads `<root>.lock`, if there is one: each line `name source ref commit`,
// tab-separated, after `//` comments.
inline void LoadLock(const string &rootpath) {
    package_lock.clear();
    package_lockdirty = false;
    auto dot = rootpath.find_last_of('.');
    auto slash = rootpath.find_last_of("/\\");
    package_lockfile = cat(dot == string::npos || (slash != string::npos && dot < slash)
                               ? rootpath : rootpath.substr(0, dot), ".lock");
    string text;
    if (!LoadFile(package_lockfile, text)) return;
    size_t at = 0;
    while (at < text.size()) {
        auto nl = text.find('\n', at);
        auto line = text.substr(at, nl == string::npos ? string::npos : nl - at);
        at = nl == string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        vector<string> f;
        size_t p = 0;
        for (;;) {
            auto t = line.find('\t', p);
            f.push_back(line.substr(p, t == string::npos ? string::npos : t - p));
            if (t == string::npos) break;
            p = t + 1;
        }
        if (f.size() == 4) package_lock[f[0]] = { f[1], f[2], f[3] };
    }
}

// Writes the lockfile where anything in it changed.
inline void SaveLock() {
    if (!package_lockdirty || package_lockfile.empty()) return;
    auto base = package_lockfile.substr(package_lockfile.find_last_of("/\\") + 1);
    string text = cat("// The packages the program declares from git, as fetched: name, source,\n"
                      "// ref, commit. Builds take these until `gust fetch --update` resolves them\n"
                      "// again. Generated (", base, "): keep it with the program.\n");
    for (auto &[name, e] : package_lock) Append(text, name, "\t", e.source, "\t", e.ref, "\t", e.commit, "\n");
    // Whole or not at all, beside builds of the same program writing the
    // same: one that cannot write it leaves it to the next.
    auto tmp = cat(package_lockfile, ".tmp", Hex8(cat((uintptr_t)&text, package_lockfile)));
    if (WriteFileMakingDirs(tmp, text) && !RenamePath(tmp, package_lockfile)) {
        package_lockdirty = false;
        return;
    }
    std::error_code ec;
    filesystem::remove(tmp, ec);
}

// The highest tag of the repository the range allows, read with
// `git ls-remote`.
inline string ResolveRange(const PackageSource &s, const string &name) {
    if (package_nofetch)
        throw CompileError { cat("the package ", name, " from ", s.text, " has a version range that ",
                                 "only the repository can resolve, and --offline asks it nothing: "
                                 "`goose fetch` resolves it into the lockfile") };
    auto git = FindOnPath("git");
    if (git.empty()) throw CompileError { cat("cannot find `git` on PATH, needed to resolve ", s.text) };
    auto r = RunProcess({ git, "ls-remote", "--tags", "--refs", s.url });
    auto out = r.output;
    if (!r.started || r.code != 0) throw CompileError { cat("cannot list the tags of ", s.url, ":\n", out) };
    string best;
    Version bestv;
    vector<string> tags;
    size_t at = 0;
    while (at < out.size()) {
        auto nl = out.find('\n', at);
        auto line = out.substr(at, nl == string::npos ? string::npos : nl - at);
        at = nl == string::npos ? out.size() : nl + 1;
        auto p = line.find("refs/tags/");
        if (p == string::npos) continue;
        auto tag = line.substr(p + 10);
        while (!tag.empty() && (tag.back() == '\r' || tag.back() == ' ')) tag.pop_back();
        auto v = TagVersion(tag);
        if (v.major < 0) continue;
        tags.push_back(tag);
        if (InRange(v, s.ref) && (best.empty() || Before(bestv, v))) {
            best = tag;
            bestv = v;
        }
    }
    if (best.empty()) {
        string list;
        for (auto &t : tags) Append(list, list.empty() ? "" : ", ", t);
        throw CompileError { cat("no tag of ", s.url, " is in the range ", s.ref, " (its version tags: ",
                                 list.empty() ? string("none") : list, ")") };
    }
    return best;
}

// The commit a checkout is at.
inline string CheckoutCommit(const string &dir) {
    auto git = FindOnPath("git");
    if (git.empty()) return "";
    auto r = RunProcess({ git, "-C", dir, "rev-parse", "HEAD" });
    if (!r.started || r.code != 0) return "";
    auto out = r.output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

// The checkout of a git source as the lockfile has it, else as its ref
// resolves now, which the lockfile then records.
inline string LockedCheckout(const PackageSource &source, const string &name) {
    auto it = package_lock.find(name);
    auto locked = it != package_lock.end() && it->second.source == source.text && !package_update;
    auto s = source;
    if (locked) s.ref = it->second.ref;
    else if (IsVersionRange(source.ref)) s.ref = ResolveRange(source, name);
    auto dir = FetchPackage(s, name);
    auto commit = CheckoutCommit(dir);
    if (locked && !commit.empty() && commit != it->second.commit) {
        // The ref moved in the cache, which another program updated: this
        // one stays at its commit.
        auto c = s;
        c.ref = it->second.commit;
        return FetchPackage(c, name);
    }
    if (!locked && !commit.empty()) {
        auto &e = package_lock[name];
        if (e.source != source.text || e.ref != s.ref || e.commit != commit) {
            e = { source.text, s.ref, commit };
            package_lockdirty = true;
        }
    }
    return dir;
}

// Gust: a registry (docs/packages.md): `import name from "@^1.2";` takes the
// repository of the package `name` from the first registry GUST_REGISTRY
// names that lists it, with the ref or range written after the `@`.
// GUST_REGISTRY is a comma-separated list of registries, each a directory
// or a git repository (as a root file writes one, without a ref) whose
// `index.txt` has a line `name source` per package, the source as a root
// file writes it, without a ref; `#` starts a comment. A git registry is
// cloned into GOOSE_HOME/registry/ once, and fetched again by `gust fetch
// --update`. What a name resolves to goes through the lockfile as any git
// source does, so the lock pins the repository and commit, not the
// registry's word for them.
inline string RegistryRepoUrl(const string &reg) {
    if (reg.find("://") != string::npos || reg.find('@') != string::npos) return reg;
    auto slashes = (int)std::count(reg.begin(), reg.end(), '/');
    if (slashes == 1) return cat("https://github.com/", reg);
    if (slashes >= 2) return cat("https://", reg);
    throw CompileError { cat("the registry ", reg, " is not a directory, owner/repo, host/owner/repo or a URL") };
}

// The directory a registry's index.txt is in: the registry itself, or its
// clone in the cache.
inline string RegistryDir(const string &reg) {
    std::error_code ec;
    if (IsPathSource(reg) || filesystem::is_directory(filesystem::path(reg), ec)) return PackageDir(reg);
    auto url = RegistryRepoUrl(reg);
    // The repository's last name and a hash of the URL, short, as a
    // package's checkout is named where its URL is long.
    auto key = url;
    while (!key.empty() && (key.back() == '/' || key.back() == '\\')) key.pop_back();
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".git") == 0) key.resize(key.size() - 4);
    key = key.substr(key.find_last_of("/\\:") + 1);
    for (auto &c : key)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) c = '_';
    auto dir = cat(GooseHome(), "/registry/", key.empty() ? string("registry") : key.substr(0, 24), "-",
                   Hex8(url));
    auto have = filesystem::is_directory(filesystem::path(dir), ec);
    if (have && !package_update) return dir + "/";
    if (!have && package_nofetch)
        throw CompileError { cat("the registry ", reg, " is not fetched, and --offline fetches nothing: "
                                 "`gust fetch` fetches it") };
    if (have && package_nofetch) return dir + "/";
    auto git = FindOnPath("git");
    if (git.empty()) throw CompileError { cat("cannot find `git` on PATH, needed to fetch the registry ", reg) };
    filesystem::create_directories(filesystem::path(dir).parent_path(), ec);
    FileLock lock(dir + ".lock", cat("waiting for another gust fetching the registry ", reg));
    ProgressStep step(cat(have ? "updating" : "fetching", " the registry ", reg), true);
    if (have) {
        RunGit(git, { "-C", dir, "fetch", "-q", "--depth", "1", url, "HEAD" });
        RunGit(git, { "-C", dir, "checkout", "-q", "--detach", "FETCH_HEAD" });
    } else {
        auto tmp = dir + ".tmp";
        RemoveTree(tmp);
        RunGit(git, { "clone", "-q", "--depth", "1", url, tmp });
        filesystem::rename(filesystem::path(tmp), filesystem::path(dir), ec);
        if (ec) throw CompileError { cat("cannot move the registry's clone to ", dir, ": ", ec.message()) };
    }
    return dir + "/";
}

// The source `import name from "@ref"` means: the registry's for name, then
// the ref (range) as written. An index that gives a directory gives it
// without a ref, relative to the registry where it is relative.
inline string RegistrySource(const string &name, const string &text) {
    auto env = getenv("GUST_REGISTRY");
    if (!env || !*env)
        throw CompileError { cat("the source ", text, " takes the package ", name, "'s repository from a "
                                 "registry, and GUST_REGISTRY names none: set it to a registry's directory or "
                                 "git repository (docs/packages.md)") };
    auto split = [](const string &all, char sep) {
        vector<string> parts;
        for (size_t at = 0;;) {
            auto p = all.find(sep, at);
            parts.push_back(all.substr(at, p == string::npos ? string::npos : p - at));
            if (p == string::npos) return parts;
            at = p + 1;
        }
    };
    auto trim = [](string t) {
        while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
        size_t i = 0;
        while (i < t.size() && isspace((unsigned char)t[i])) i++;
        return t.substr(i);
    };
    for (auto &raw : split(env, ',')) {
        auto reg = trim(raw);
        if (reg.empty()) continue;
        auto dir = RegistryDir(reg);
        string index;
        if (!LoadFile(cat(dir, "index.txt"), index))
            throw CompileError { cat("the registry ", reg, " has no index.txt") };
        for (auto &line : split(index, '\n')) {
            auto l = line;
            if (auto h = l.find('#'); h != string::npos) l.resize(h);
            auto t = trim(l);
            auto sp = t.find_first_of(" \t");
            if (sp == string::npos || t.substr(0, sp) != name) continue;
            auto source = trim(t.substr(sp));
            if (IsPathSource(source)) return IsAbsoluteSource(source) ? source : cat(dir, source);
            return source + text;
        }
    }
    throw CompileError { cat("no registry GUST_REGISTRY names (", env, ") lists the package ", name) };
}

// The root directory of the package `name` from `source`: the directory
// itself for a path, the checkout in the cache for a git source; either
// must hold `<name>.goose`. Gust: a declared git package is the lockfile's
// (`lockable`); a blessed one is pinned by the compiler's own table.
inline string PackageRoot(const PackageSource &source, const string &name, bool lockable = true) {
    string dir;
    if (source.ispath) {
        std::error_code ec;
        if (!filesystem::is_directory(filesystem::path(source.dir), ec))
            throw CompileError { cat("the package ", name, "'s source ", source.text,
                                     " is not a directory (", source.dir, ")") };
        dir = source.dir;
    } else {
        dir = !lockable || package_lockfile.empty() || IsCommitHash(source.ref)
                  ? FetchPackage(source, name) : LockedCheckout(source, name);
    }
    if (!PathExists(SourceFile(cat(dir, name))))
        throw CompileError { cat("the package ", name, " at ", dir, " has no ", name, ".goose (or ", name, ".gust)") };
    return dir;
}

// The source as the root file in `rootdir` would write it, for the error
// that tells the root what to add: a path relative to there where that can
// be said, else as it is.
inline string SourceForRoot(const PackageSource &source, const string &rootdir) {
    if (!source.ispath) return source.text;
    auto rel = ForwardSlashes(RelativeTo(source.dir.substr(0, source.dir.size() - 1),
                                         PackageDir(rootdir.empty() ? string(".") : rootdir)));
    if (rel == source.dir.substr(0, source.dir.size() - 1)) return rel;   // No relative form.
    if (rel.rfind("../", 0) != 0 && rel.rfind("./", 0) != 0) rel = cat("./", rel);
    return rel;
}

}  // namespace goose
