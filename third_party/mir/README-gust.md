# MIR in Gust

[MIR](https://github.com/vnmakarov/mir), Vladimir Makarov's JIT compiler
(MIT, `LICENSE`), at commit `a8ab7c31cd5f9b23b77d84c60b3d83e62d9d304c`: the
subset Lobster vendors in its `dev/external/mir`, copied from a Lobster
checkout. It is what is needed to compile C into memory: `mir.c` (the IR and
the interpreter `MIR_link` needs), `mir-gen.c` (the code generator, for
x86-64 and arm64) and `c2mir/c2mir.c` (the C front end), with every file
they include. Gust uses it for `gust run --mir` (`src/jit_mir.h`).

Gust's changes are marked `Gust` where they are and listed in
`MODIFICATIONS.md` at the repository's root. To update, copy the same files
from a newer MIR checkout and carry the changes over.
