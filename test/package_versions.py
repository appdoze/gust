"""Gust: version ranges and the lockfile (docs/packages.md, src/packages.h),
offline, against a bare repository this runner makes with version tags: a
range resolves to the highest tag it allows and is recorded with its commit
in `program.lock`; a newer tag leaves the program where the lock has it
until `gust fetch --update`; a program whose lock names an older commit of a
branch than the shared cache now holds stays at its commit; a range no tag
satisfies says which tags there are; --offline resolves no range, but takes
one the lock has."""
import os
import shutil
import subprocess
from pathlib import Path

import toolchain as tc


def check_package_versions(exe, base, *, jit):
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
    base = (Path(base) / "package-versions").resolve()
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

    def release(code, tag=None):
        tc.write_text(work / "verpkg.goose", f"namespace verpkg;\nfn code() -> i64 {{ {code} }}\n")
        run_git("add", ".")
        run_git("commit", "-q", "-m", f"code {code}")
        if tag:
            run_git("tag", tag)
        return run_git("rev-parse", "HEAD")

    run_git("init", "-q", "-b", "main", ".")
    release(100, "v1.0.0")
    release(120, "v1.2.0")
    release(200, "2.0.0")
    release(90, "not-a-version")
    bare = base / "verpkg.git"
    run_git("clone", "-q", "--bare", str(work), str(bare))
    run_git("config", "uploadpack.allowAnySHA1InWant", "true", cwd=bare)
    url = "file:///" + bare.as_posix().lstrip("/")

    def push():
        run_git("push", "-q", "--tags", str(bare), "main")

    def program(name, ref):
        path = base / f"{name}.goose"
        tc.write_text(path, f'import verpkg from "{url}@{ref}";\nfn main() {{ print(verpkg::code()); }}\n')
        return path

    env = dict(os.environ, GIT_ALLOW_PROTOCOL="file", GOOSE_HOME=str(home))

    def gust(*args, ok=True):
        r = subprocess.run([exe, *args], cwd=base, env=env, capture_output=True, timeout=120)
        out, err = tc.decode(r.stdout), tc.decode(r.stderr)
        if ok and (r.returncode != 0 or tc.sanitizer_failure(err)):
            raise RuntimeError(f"gust {' '.join(str(a) for a in args)} (exit {r.returncode}):\n{out}{err}")
        return r.returncode, out, err

    def expect(path, want):
        if jit:
            _, out, _ = gust("run", "-q", path)
            if out.strip() != str(want):
                raise RuntimeError(f"{path.name}: printed {out.strip()!r}, wanted {want}")
        else:
            gust("check", path)

    def lock_of(path):
        text = path.with_suffix(".lock").read_text(encoding="utf-8")
        rows = [l.split("\t") for l in text.splitlines() if l and not l.startswith("//")]
        return {r[0]: r for r in rows}

    caret = program("caret", "^1.0")
    expect(caret, 120)
    row = lock_of(caret)["verpkg"]
    if row[2] != "v1.2.0" or len(row[3]) != 40:
        raise RuntimeError(f"caret.lock should hold v1.2.0 and its commit: {row}")
    expect(program("tilde", "~1.0.0"), 100)
    expect(program("atleast", ">=1.1"), 200)
    expect(program("wild", "1.x"), 120)

    # A newer tag in range: the lock holds until --update.
    release(130, "v1.3.0")
    push()
    expect(caret, 120)
    gust("fetch", "--update", caret)
    expect(caret, 130)
    if lock_of(caret)["verpkg"][2] != "v1.3.0":
        raise RuntimeError("fetch --update should have moved caret.lock to v1.3.0")

    # A branch: another program's --update moves the shared checkout, and
    # this one stays at the commit its lock names.
    branch = program("branch", "main")
    expect(branch, 130)
    release(140)
    push()
    other = program("other", "main")
    gust("fetch", "--update", other)
    expect(other, 140)
    expect(branch, 130)

    # No tag in range, and --offline.
    code, out, err = gust("check", program("none", "^3.0"), ok=False)
    if code == 0 or "no tag of" not in out + err or "v1.2.0" not in out + err:
        raise RuntimeError(f"a range no tag satisfies should list the tags:\n{out}{err}")
    code, out, err = gust("check", "--offline", program("fresh", "^1.1"), ok=False)
    if code == 0 or "--offline" not in out + err:
        raise RuntimeError(f"--offline should not resolve a range:\n{out}{err}")
    gust("check", "--offline", caret)
    return None
