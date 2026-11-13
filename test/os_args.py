"""Gust: a program's arguments reach it as UTF-8 whatever the system's code page
holds, on Windows too, where main's argv has lost what the page cannot spell.
Built and run in process, with the shapes the C runtime's splitting treats
specially, and a file opened by a name argv alone could not name."""
import subprocess
from pathlib import Path

SOURCE = r'''
import os;

fn main() {
    let a = args();
    for i in 1..a.len {
        var line: u8[>..] = [];
        for b in a[i] {
            line.push("0123456789abcdef"[(b >> 4) as i64]);
            line.push("0123456789abcdef"[(b & 15) as i64]);
        }
        print("[", line, "]");
    }
    var d: u8[>..] = [];
    print(read_file(a[a.len - 1], d), " ", d);
}
'''

ARGS = ["日本語", "plain", "with space", 'quo"te', "back\\slash", "trail\\", 'both\\"', "é", "🐟", ""]


def expected(args, body):
    lines = [f"[{a.encode('utf-8').hex()}]" for a in args]
    lines.append(f"true {body}")
    return lines


def check_os_args(exe, gendir, run_in_process, built):
    base = Path(gendir) / "os_args"
    base.mkdir(parents=True, exist_ok=True)
    src = base / "os_args.gust"
    src.write_text(SOURCE, encoding="utf-8")
    named = base / "ファイル-ωmega.txt"
    named.write_text("found", encoding="utf-8")
    args = ARGS + [str(named)]
    want = expected(args, "found")

    def compare(what, cmd):
        r = subprocess.run(cmd, capture_output=True, timeout=300)
        got = r.stdout.decode("utf-8", "replace").split()
        flat = " ".join(want).split()
        if r.returncode or got != flat:
            raise RuntimeError(f"{what}: exit {r.returncode}\ngot  {got}\nwant {flat}\n"
                               f"{r.stderr.decode('utf-8', 'replace')}")

    if run_in_process:
        compare("gust run", [str(exe), "run", "-q", str(src), "--", *args])
    if built:
        out = base / "os_args_prog"
        r = subprocess.run([str(exe), "build", "-q", str(src), "-o", str(out)], capture_output=True,
                           timeout=300)
        if r.returncode:
            raise RuntimeError(f"gust build: {r.stdout.decode()}{r.stderr.decode()}")
        compare("built", [str(out), *args])
