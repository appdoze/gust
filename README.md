# Gust

**Rust-grade safety, Goose-grade speed.**

Gust is a memory-safe systems language with no heap allocator, no garbage
collector and no lifetime annotations. It is a fork of
[Goose](https://github.com/aardappel/goose) by Wouter van Oortmerssen, and
aims to be faster than Rust across the board while staying as safe: safe
parallelism, access to large C libraries, and automatic vectorization are
next ([ROADMAP.md](ROADMAP.md)).

The name blends the two languages it sits between: **G**oose's memory model
and the safety bar of R**ust**.

## Status

Early fork. The language today is Goose's, with these changes
([MODIFICATIONS.md](MODIFICATIONS.md) has the full list):

- The command is `gust`, and source files may be `.gust` (or `.goose`, which
  the standard library and the shared test suite keep).
- On Windows, `gust build` finds clang where the LLVM installer puts it, and
  builds CMake packages correctly under clang. clang's code was 17-28% faster
  than MSVC's on float-heavy benchmarks.
- Its cache lives in `~/.gust`, apart from any Goose install.

Everything in [GOOSE.md](GOOSE.md) (Goose's overview) and under
[docs/](docs/index.md) applies to Gust; the documentation still uses Goose's
name. So does the VS Code extension in [vscode/](vscode/README.md), which
edits `.goose` files: set `goose.compilerPath` to the `gust` binary.

## How it compares

Measured on one Windows machine (Intel i7-11700F) against Rust 1.96 on eight
checksum-verified benchmarks: five from Lobster's cross-language suite (n-body,
ray tracing, an AST interpreter, quicksort, A*) plus binary trees, a hash map
and string processing.

| | vs Rust (`-C opt-level=3`, LTO) |
|---|---|
| time, all 8 benchmarks, Goose + clang | 0.59x (about 1.7x faster) |
| time, the 5 compute benchmarks | 0.95x |
| n-body and ray tracing | level |
| peak memory | 18% less |

The goal for Gust 1.0 is to be ahead on every one of them; the roadmap says
how.

## Build and run

You need CMake 3.27 or later, a C++20 compiler and Python 3; clang (from
[LLVM](https://github.com/llvm/llvm-project/releases)) is the recommended C
compiler for the programs Gust builds.

```bash
git clone https://github.com/appdoze/gust
cd gust
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The compiler is `build/gust` (`build/Release/gust.exe` with Visual Studio).

```bash
build/gust samples/01_tour.goose
build/gust build hello.gust && ./hello
python test/run_tests.py
```

`gust` with no verb compiles and runs a program in-process; `gust build`
makes an executable with the C compiler it finds (`gust toolchain` says
which); `gust check` only typechecks. `gust --help` lists the rest.

## License and credits

Gust is licensed under the [Apache License, Version 2.0](LICENSE), as Goose
is. Files changed for Gust say so at their top, and
[MODIFICATIONS.md](MODIFICATIONS.md) lists the changes.

- **Goose**, which Gust is built on: language design by Wouter van
  Oortmerssen, implementation with Claude Fable ([GOOSE.md](GOOSE.md),
  [upstream](https://github.com/aardappel/goose)).
- **Gust**: [appdoze](https://github.com/appdoze).
- TinyCC, which the compiler embeds for in-process runs, is LGPL-2.1
  ([third_party/tinycc/COPYING](third_party/tinycc/COPYING)); a binary
  distribution of the compiler must let users relink it with their own
  TinyCC. The programs Gust builds with clang, gcc or MSVC do not contain it.
