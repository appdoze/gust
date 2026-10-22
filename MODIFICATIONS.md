# Modifications

Gust is Goose (https://github.com/aardappel/goose, Apache-2.0) with the
changes below. Every file that differs from Goose's carries a notice at its
top pointing here; `git log upstream/master..main` has
the exact changes.

## Toolchain

- `src/toolchain.h`: on Windows, also look for clang in
  `%ProgramFiles%\LLVM\bin`, where the LLVM installer puts it without adding
  it to PATH, before falling back to MSVC through vcvars64; the not-found
  message names that place. The default `GOOSE_HOME` is `~/.gust`.
- `src/native.h`: a CMake package's link closure is written to the response
  file with forward slashes for cc-style compilers, which read response
  files with Unix quoting (clang on Windows took `pkg\cmk.lib` for
  `pkgcmk.lib`).
- `docs/design/packages.md`: the not-found message as it now reads.

## Name

- `CMakeLists.txt`: the compiler builds as `gust` (`OUTPUT_NAME`); the CMake
  target keeps Goose's name.
- `src/main.cpp`: the help and the driver's messages call the command
  `gust` and source files `file.gust`; `gust version` prints
  `gust runtime <version>`. Imports resolve through `SourceFile`.
- `src/utils.h`: `SourceFile` and `IsSourceExtension`: a source path
  resolves to `.gust` where that file exists, else `.goose`.
- `src/builtin.h`, `src/packages.h`, `src/native.h`: package entries and
  native-part sources are found with either extension.
- `README.md` is Gust's; Goose's is kept as `GOOSE.md`. `ROADMAP.md` and this
  file are new.

## Scripts and tests

- `scripts/toolchain.py`, `scripts/check_doc_examples.py`: find a `gust`
  binary under `build/`, and a `goose` one from before the rename.
- `scripts/package_release.py`, `test/lsp_sweep.py`,
  `test/toolchain_selection.py`, `.github/workflows/ci.yml`,
  `.github/workflows/release.yml`: the binary is `gust`, and releases are
  named `gust-<version>-<platform>`.
- `test/run_tests.py`: the driver checks expect the `gust` wording and test
  `.gust` sources (a program importing a `.gust` and a `.goose` module); the
  no-C-compiler check also hides `%ProgramFiles%` and `%ProgramW6432%`.
