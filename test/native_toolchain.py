# Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
"""One C toolchain for a whole build: `goose build` of a program using a CMake
package builds the package, the runtime object and the executable with the
toolchain `goose toolchain` reports, and a JIT `goose run` builds the package
with it too. On Windows, building a CMake package imports Visual Studio's
environment, which puts cl on PATH partway through; so cl starts off it here,
as it would in a plain shell."""
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import toolchain as tc

HERE = Path(__file__).resolve().parent


def check_native_toolchain(exe, base, *, jit):
    """Returns why it was skipped, or None when every check passed; raises
    RuntimeError on a failure."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="toolchain-", dir=base) as tmp:
        root = Path(tmp)
        shutil.copytree(HERE / "packages" / "pkgs" / "cmk", root / "pkgs" / "cmk")
        source = shutil.copy2(HERE / "packages" / "pkg_cmake_lib.goose", root)
        home = root / "home"
        env = {k: v for k, v in os.environ.items() if k != "GOOSE_CC"}
        env["GOOSE_HOME"] = str(home)
        if tc.IS_WINDOWS:
            env["PATH"] = os.pathsep.join(
                d for d in env.get("PATH", "").split(os.pathsep)
                if not d or not (Path(d) / "cl.exe").is_file())

        def run(argv):
            r = subprocess.run([str(a) for a in argv], cwd=root, env=env, capture_output=True,
                               text=True, errors="replace", timeout=600)
            return r.returncode, r.stdout, r.stderr

        def require(good, message):
            if not good:
                raise RuntimeError(f"native toolchain: {message}")

        code, out, err = run([exe, "toolchain", "--json"])
        try:
            info = json.loads(out)
        except ValueError:
            raise RuntimeError(f"native toolchain: goose toolchain --json:\n{out}{err}")
        if not info.get("cc"):
            return "no C compiler"
        if not info.get("cmake"):
            return "no cmake"
        want = {info["cc"]["id"]}

        def ids(pattern, strip_config=False):
            return {p.name.rsplit("-", 1)[0] if strip_config else p.name
                    for p in home.glob(pattern) if p.is_dir()}

        app = root / ("app" + tc.EXE_SUFFIX)
        code, out, err = run([exe, "build", "-o", app, source])
        require(code == 0, f"goose build:\n{out}{err}")
        code, out, err = run([app])
        require(code == 0 and out.split() == ["12", "-15"], f"the built program:\n{out}{err}")
        packages = ids("build/pkg/local/*/*")
        runtimes = ids("build/runtime/*/*", strip_config=True)
        require(packages == want and runtimes == want,
                f"goose build used toolchains {sorted(packages)} for the package and "
                f"{sorted(runtimes)} for the runtime, where goose toolchain reports "
                f"{sorted(want)}")
        if jit:
            code, out, err = run([exe, "run", source])
            require(code == 0 and out.split() == ["12", "-15"], f"goose run:\n{out}{err}")
            packages = ids("build/pkg/local/*/*")
            require(packages == want,
                    f"goose run used toolchains {sorted(packages - want)} for the package, "
                    f"where goose toolchain reports {sorted(want)}")
    return None


def check_cmake_response_files(exe, base):
    """Build and run a CMake interface package through a cc-style response
    file. MinGW needs a separate check from the default Windows toolchain,
    which can be MSVC and does not consume backslashes in the same way."""
    cc = shutil.which("gcc" if tc.IS_WINDOWS else "cc")
    if not cc:
        return "no gcc on PATH" if tc.IS_WINDOWS else "no cc on PATH"
    if not shutil.which("cmake"):
        return "no cmake on PATH"
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cmake-response-", dir=base) as tmp:
        root = Path(tmp)
        shutil.copytree(HERE / "packages" / "pkgs" / "cmk", root / "pkgs" / "cmk")
        source = shutil.copy2(HERE / "packages" / "pkg_cmake_lib.goose", root)
        app = root / ("app" + tc.EXE_SUFFIX)
        for name in ("home", "home with spaces"):
            env = dict(os.environ, GOOSE_HOME=str(root / name))
            for argv, want in (([exe, "build", "--cc", cc, "-o", app, source], None),
                               ([app], ["12", "-15"])):
                result = subprocess.run([str(a) for a in argv], cwd=root, env=env,
                                        capture_output=True, text=True, errors="replace", timeout=600)
                if result.returncode or (want is not None and result.stdout.split() != want):
                    raise RuntimeError(f"CMake response file ({name}): exit {result.returncode}\n"
                                       f"{result.stdout}{result.stderr}")
    return None


def check_default_prefers_clang(exe, base):
    """Gust: with no --cc, GOOSE_CC or configured compiler, discovery takes
    clang wherever it is installed (on Windows, once cl is off PATH, as in a
    plain shell). Returns why it was skipped, or None."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="toolchain-default-", dir=base) as tmp:
        env = {k: v for k, v in os.environ.items() if k != "GOOSE_CC"}
        env["GOOSE_HOME"] = str(Path(tmp) / "home")
        if tc.IS_WINDOWS:
            env["PATH"] = os.pathsep.join(
                d for d in env.get("PATH", "").split(os.pathsep)
                if not d or not (Path(d) / "cl.exe").is_file())
        r = subprocess.run([str(exe), "toolchain", "--json"], env=env, capture_output=True, text=True,
                           errors="replace", timeout=600)
        try:
            info = json.loads(r.stdout)
        except ValueError:
            raise RuntimeError(f"default toolchain: goose toolchain --json:\n{r.stdout}{r.stderr}")
        cc = info.get("cc")
        if not cc:
            return "no C compiler"
        if tc.IS_WINDOWS:
            if cc["name"] not in ("clang", "clang-cl"):
                found = shutil.which("clang", path=env["PATH"]) or shutil.which("clang-cl", path=env["PATH"])
                if found:
                    raise RuntimeError(f"default toolchain: {cc['desc']} chosen where clang is at {found}")
                return "no clang"
            return None
        if not shutil.which("clang", path=env.get("PATH")):
            return "no clang on PATH"
        if cc["name"] != "clang":
            raise RuntimeError(f"default toolchain: {cc['desc']} ({cc['how']}) chosen where clang is on PATH")
    return None

