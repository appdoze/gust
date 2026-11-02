# Gust

[![CI](https://github.com/appdoze/gust/actions/workflows/ci.yml/badge.svg)](https://github.com/appdoze/gust/actions/workflows/ci.yml)

**Rust-grade safety, Goose-grade speed.**

Gust is a memory-safe systems language with no heap allocator, no garbage
collector and no lifetime annotations. It is a fork of
[Goose](https://github.com/aardappel/goose) by Wouter van Oortmerssen, and
aims to be faster than Rust across the board while staying as safe. Better
generated code, access to large C libraries, safe parallelism and automatic
vectorization are next ([ROADMAP.md](ROADMAP.md)).

The name is the two languages it sits between: G(oose) + (R)ust.

## Status

Early fork. The language today is Goose's, with these changes
([MODIFICATIONS.md](MODIFICATIONS.md) has the full list):

- The command is `gust`, and source files may be `.gust` or `.goose`. The
  standard library, the samples and the test suite keep `.goose`, which Gust
  shares with Goose.
- On Windows, `gust build` finds clang where the LLVM installer puts it, and
  builds CMake packages correctly under clang. MSVC's code was 17-27% slower
  than clang's on the float-heavy benchmarks.
- `--checked` builds optimized programs that still abort on integer overflow,
  where Goose and Rust wrap silently in release builds. It costs about 5% on
  average (17% at most) across the benchmarks below.
- `--externs` lists the C functions a program calls outside the standard
  library, which the safety rules cannot see into, and where a struct they
  take is laid out differently in C; `--deny-externs` refuses them, as
  auditing Rust's `unsafe` would. A sidecar's `c_headers` has the C compiler
  check each `extern fn` against its header.
- `c_callback(f)` hands C an export fn to call back (qsort's comparator,
  libcurl's write callback), and `scripts/gust_python.py` turns a library's
  exports into a pip-installable Python package
  ([reference](docs/reference.md#python-bindings-gust)).
- A thread's data stacks commit memory as they grow from 16 KB, not 1 MB at
  a time, so a server with a thread per connection stays small.
- An HTTP/1.1 client, [`http`](docs/stdlib.md#http), and a standard library module for TCP, [`net`](docs/stdlib.md#net):
  non-blocking sockets behind checked handles, and an event loop for
  servers with bounded output, streaming and backpressure. `slots` gives
  `reusable` pools generation-checked handles.
- [examples/api/](examples/api/README.md) is a JSON API over HTTP/1.1 with
  FastAPI-style validation errors, served on `net` by a pool of event-loop
  workers and measured against Go; [examples/echo/](examples/echo/echo.gust)
  is a TCP echo server on the same loop.
- Its cache lives in `~/.gust`, apart from any Goose install.

Design notes for these are in [docs/gust/](docs/gust/): the
[`--checked` and `extern fn` audit](docs/gust/checked_and_externs.md), the
[compiler architecture](docs/gust/compiler_architecture.md) (why Gust keeps
generating C), [backends and the API benchmark](docs/gust/backend_api.md),
[what other languages do](docs/gust/language_research.md), and
[rust-lang/rust#164250 and Gust](docs/gust/soundness_rust_164250.md), and the
[stability and edition policy](docs/gust/stability.md) 1.0 will follow. For
an AI coding agent, [the language in one sitting](docs/gust/agent_guide.md),
in a single file to load whole.

Not renamed yet, so that Goose's changes keep merging: the documentation in
[GOOSE.md](GOOSE.md) (Goose's overview) and [docs/](docs/index.md), which
applies to Gust as written; the environment variables (`GOOSE_HOME`,
`GOOSE_CC`, `GOOSE_STDLIB`); the runtime's messages (`goose runtime error:
...`); and the VS Code extension in [vscode/](vscode/README.md), which edits
`.goose` files and finds the compiler through its `goose.compilerPath`
setting.

## How it compares

Gust's compiler with clang against Rust (`-C opt-level=3`, fat LTO), on GitHub's
hosted runners, the fastest of seven alternating runs of each, over eight
benchmarks whose checksums match in both languages: five from Lobster's
cross-language suite (n-body, ray tracing, an AST interpreter, quicksort, A*)
plus binary trees, a hash map and string processing. Gust is ahead on all eight
on both systems. `compare/` has the programs and the harness; these are run
[41344580435](https://github.com/appdoze/gust/actions/runs/41344580435).

| measure, Gust relative to Rust | Ubuntu | Windows |
|---|---|---|
| time over all 8 (geometric mean) | 0.50x | 0.45x |
| closest of the 8 | ray tracing and quicksort, 0.95x | ray tracing, 0.92x |
| peak memory (geometric mean) | 0.72x | 0.84x |
| the benchmark program's size | 72 KB, Rust 2.15 MB | 85 KB, Rust 188 KB |
| hello world, start to exit | 0.56 ms, Rust 0.73 ms, Go 1.00 ms | 4.9 ms, Rust 5.1 ms, Go 6.8 ms |

Serving a JSON API, mixed load over 64 connections on a 4-CPU Linux runner, one
3-second run on a shared machine, so indicative only (run
[41357916334](https://github.com/appdoze/gust/actions/runs/41357916334)):

| server | requests/s | p99 | peak memory |
|---|---:|---:|---:|
| Gust, `http::serve` | 157k | 1.05 ms | 2.4 MB |
| Rust on tokio, the same minimal HTTP | 149k | 0.92 ms | 3.9 MB |
| Go, the same minimal HTTP | 117k | 1.48 ms | 9.5 MB |
| Go's net/http | 55k | 3.29 ms | 12.8 MB |

Where it is behind: tokio has the lower p99 there and answers a health check 3%
faster, and with a new connection per request Gust's 17.6k a second is 5 to 6% under
Go's and tokio's. Sorting integers takes 1.15x to 1.35x Rust's time, gcc builds
are behind Rust on sort, A* and ray tracing, and an MSVC build on ray tracing. Go's
goroutines start a hundred at once faster than threads can, 20x on Linux, and a
sender with three receivers at one queue on Linux takes about 2x what Go does and
1.2x to 1.8x what Rust does. [ROADMAP.md](ROADMAP.md) has the rest.

A static build, `gust build --target x86_64-linux-musl`, resides in 88 KB for a hello world,
against about 2 MB for Rust, Go and Gust's usual build, whose C library's pages are shared between
processes. It starts in 0.17 ms, against 0.49 ms, and needs nothing installed where it runs.

## Build and run

You need CMake 3.27 or later and a C++20 compiler; Python 3 runs the tests.
clang (from [LLVM](https://github.com/llvm/llvm-project/releases)) is the
recommended C compiler for the programs Gust builds.

```bash
git clone https://github.com/appdoze/gust
cd gust
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

With [Zig](https://ziglang.org/download) on PATH, Gust builds for other systems from any of
them, static on Linux, with nothing else installed:

```bash
gust build --target aarch64-linux-musl app.gust
```

The compiler is `build/gust`, or `build/Release/gust.exe` where CMake uses
Visual Studio, as it does by default on Windows; use that path in place of
`build/gust` below.

Run a sample in-process, with no build step:

```bash
build/gust samples/01_tour.goose
```

A program of your own, in `hello.gust`:

```
fn main() {
    print("hello, gust");
}
```

```bash
build/gust hello.gust
build/gust build hello.gust
./hello
```

`gust` with no verb (or `gust run`) compiles and runs a program in-process;
`gust build` makes an executable with the C compiler it finds (`gust
toolchain` says which); `gust check` only typechecks. `gust --help` lists the
rest.

The test suite needs a C compiler for most of its checks:

```bash
python test/run_tests.py -j 8
```

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
  TinyCC, which configuring with `-DGUST_TCC_SHARED=ON` provides: TinyCC is
  then a shared library beside the compiler (`gusttcc.dll`, `libgusttcc.so`,
  `libgusttcc.dylib`) that a user can replace. The programs Gust builds with
  clang, gcc or MSVC do not contain it.
- MIR, Vladimir Makarov's JIT compiler, which `gust run --mir` uses, is MIT
  ([third_party/mir/LICENSE](third_party/mir/LICENSE)).
