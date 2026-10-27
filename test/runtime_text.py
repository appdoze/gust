"""Gust: src/runtime_inline.h, the runtime's text the compiler embeds and
writes into every program, against the files of src/runtime/ it is made from
(`gust --gen-runtime-header`). The runtime compiled into the compiler for
gust run --mir (src/runtime_native.c) includes those files themselves, so a
header left behind after an edit would hand a --mir program a runtime other
than the one its unit was written for."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# One entry per file: its name, then its text as raw string chunks (a long
# file is split, for compilers that cap a literal's length).
ENTRY = re.compile(r'\{ "([^"]+)", string_view\(\n((?:R"GSRT\(.*?\)GSRT"\n)+)    \) \},', re.S)
CHUNK = re.compile(r'R"GSRT\((.*?)\)GSRT"\n', re.S)


def check_runtime_text():
    """The number of files compared; raises RuntimeError on a difference."""
    header = (ROOT / "src" / "runtime_inline.h").read_text(encoding="utf-8")
    entries = ENTRY.findall(header)
    if not entries:
        raise RuntimeError("src/runtime_inline.h holds no runtime files that this check can read")
    stale = []
    for name, chunks in entries:
        embedded = "".join(CHUNK.findall(chunks))
        source = (ROOT / "src" / "runtime" / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        if embedded != source:
            stale.append(name)
    listed = {name for name, _ in entries}
    missing = sorted(p.name for p in (ROOT / "src" / "runtime").glob("*.h") if p.name not in listed)
    if stale or missing:
        raise RuntimeError("src/runtime_inline.h is not src/runtime/ as it is"
                           + (f": {', '.join(stale)} differ" if stale else "")
                           + (f"; it leaves out {', '.join(missing)}" if missing else "")
                           + "; run gust --gen-runtime-header and rebuild")
    return len(entries)


if __name__ == "__main__":
    print(f"{check_runtime_text()} runtime files embedded as they are")
