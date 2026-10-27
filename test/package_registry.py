"""Gust: package registries (docs/packages.md, src/packages.h's
RegistrySource), offline: `import name from "@^1.0";` takes the package's
repository from the first registry GUST_REGISTRY names that lists it, and
the range after the `@` resolves as any git source's does, through the
lockfile. Registries here are a directory and a git repository this runner
makes, listing a bare repository with version tags and a directory; a list
of two finds a package in the second; no registry, or none listing the
name, says so."""
import os
import shutil
import subprocess
from pathlib import Path

import toolchain as tc


def check_package_registry(exe, base, *, jit):
    """Returns why it was skipped, or None when every check passed; raises
    RuntimeError on a failure."""
    git = shutil.which("git")
    if not git:
        return "no git on PATH"

    def rmtree(path):
        if not path.exists():
            return
        for p in path.rglob("*"):
            try:
                p.chmod(0o777)
            except OSError:
                pass
        shutil.rmtree(path)
    base = (Path(base) / "package-registry").resolve()
    rmtree(base)
    work = base / "work"
    work.mkdir(parents=True)
    home = base / "home"

    def run_git(*args, cwd=work):
        r = subprocess.run([git, "-c", "user.name=gust", "-c", "user.email=gust@example.com",
                            "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                           cwd=cwd, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
        return r.stdout.strip()

    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        tc.write_text(path, text)

    def url_of(path):
        return "file:///" + path.as_posix().lstrip("/")

    # The package: three releases.
    run_git("init", "-q", "-b", "main", ".")
    for code, tag in ((100, "v1.0.0"), (110, "v1.1.0"), (200, "v2.0.0")):
        write(work / "regpkg.goose", f"namespace regpkg;\nfn code() -> i64 {{ {code} }}\n")
        run_git("add", ".")
        run_git("commit", "-q", "-m", f"code {code}")
        run_git("tag", tag)
    bare = base / "regpkg.git"
    run_git("clone", "-q", "--bare", str(work), str(bare))

    # A directory registry, listing it and a package of its own in place.
    reg = base / "reg"
    write(reg / "local" / "localpkg" / "localpkg.goose", "namespace localpkg;\nfn word() -> i64 { 7 }\n")
    index = f"# a registry for the test\nregpkg  {url_of(bare)}\n\nlocalpkg ./local/localpkg   # in place\n"
    write(reg / "index.txt", index)
    # The same index in a git repository of its own.
    regrepo = base / "regrepo"
    write(regrepo / "index.txt", f"regpkg {url_of(bare)}\n")
    run_git("init", "-q", "-b", "main", ".", cwd=regrepo)
    run_git("add", ".", cwd=regrepo)
    run_git("commit", "-q", "-m", "index", cwd=regrepo)
    regbare = base / "regrepo.git"
    run_git("clone", "-q", "--bare", str(regrepo), str(regbare), cwd=base)
    empty = base / "empty"
    write(empty / "index.txt", "# nothing listed\n")

    def program(name, text):
        path = base / f"{name}.goose"
        write(path, text)
        return path

    def gust(prog, registry, ok=True):
        env = dict(os.environ, GIT_ALLOW_PROTOCOL="file", GOOSE_HOME=str(home))
        env.pop("GUST_REGISTRY", None)
        if registry is not None:
            env["GUST_REGISTRY"] = registry
        if jit:
            argv = [exe, "run", "-q", prog]
        else:
            out = base / (prog.stem + tc.EXE_SUFFIX)
            r = subprocess.run([exe, "build", "-q", prog, "-o", out], cwd=base, env=env, capture_output=True,
                               timeout=300)
            if r.returncode != 0:
                if ok:
                    raise RuntimeError(f"gust build {prog.name}:\n{tc.decode(r.stdout)}{tc.decode(r.stderr)}")
                return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)
            argv = [out]
        r = subprocess.run([str(a) for a in argv], cwd=base, env=env, capture_output=True, timeout=300)
        out, err = tc.decode(r.stdout), tc.decode(r.stderr)
        if ok and (r.returncode != 0 or tc.sanitizer_failure(err)):
            raise RuntimeError(f"{prog.name} with GUST_REGISTRY={registry} (exit {r.returncode}):\n{out}{err}")
        return r.returncode, out, err

    ranged = program("ranged", 'import regpkg from "@^1.0";\nfn main() { print(regpkg::code()); }\n')
    _, out, _ = gust(ranged, str(reg))
    if out.strip() != "110":
        raise RuntimeError(f"@^1.0 through the directory registry printed {out!r}, not 110")
    lock = (base / "ranged.lock").read_text(encoding="utf-8")
    if f"{url_of(bare)}@^1.0" not in lock or "v1.1.0" not in lock:
        raise RuntimeError(f"the lock should pin the repository and its tag:\n{lock}")

    local = program("local", 'import localpkg from "@";\nfn main() { print(localpkg::word()); }\n')
    _, out, _ = gust(local, str(reg))
    if out.strip() != "7":
        raise RuntimeError(f"a directory the registry lists printed {out!r}, not 7")

    major = program("major", 'import regpkg from "@^2";\nfn main() { print(regpkg::code()); }\n')
    _, out, _ = gust(major, url_of(regbare))
    if out.strip() != "200":
        raise RuntimeError(f"@^2 through the git registry printed {out!r}, not 200")
    if not (home / "registry").is_dir():
        raise RuntimeError("the git registry was not cloned into GOOSE_HOME/registry")

    (base / "ranged.lock").unlink()
    _, out, _ = gust(ranged, f"{empty}, {reg}")
    if out.strip() != "110":
        raise RuntimeError(f"a list whose second registry lists the package printed {out!r}")

    code, out, err = gust(ranged, None, ok=False)
    if code == 0 or "GUST_REGISTRY names none" not in out + err:
        raise RuntimeError(f"no registry: exit {code}\n{out}{err}")
    code, out, err = gust(ranged, str(empty), ok=False)
    if code == 0 or "lists the package regpkg" not in out + err:
        raise RuntimeError(f"a registry without the package: exit {code}\n{out}{err}")
    return None
