# Stability and editions

What Gust promises about the code you write against it, and how that changes
over time. This is the policy 1.0 will be released under (`ROADMAP.md`,
Phase 8). Until 1.0 is tagged it describes intent: the language is still
the soft fork of Goose it started as, and the spec (`docs/goose_spec.md`) is
a draft that changes with it.

## What 1.0 freezes

From 1.0 on, a program that compiles with Gust 1.x compiles with every later
1.x and does what it did:

- **The language**: what `docs/goose_spec.md` at the 1.0 tag says, Gust's
  additions marked in it included (labeled jumps, §6.4; C callbacks, §7.11).
  A construct the spec does not define is not promised, whatever the
  compiler happens to do with it.
- **The standard library's documented functions** (`docs/stdlib.md`): their
  names, parameter and result types, and documented behaviour.
- **The command line**: the commands and options `gust help` lists, the
  exit statuses they document, and the `--diagnostics json` format's fields
  (new fields may appear; none disappear or change meaning).
- **Package sources and the lockfile**: what `import name from "..."` accepts,
  the version ranges (`docs/packages.md`), and `<program>.lock`'s format.

## What is not frozen

- **Performance**: code may get faster or, within reason, slower between
  releases; `compare/` tracks it, and a regression there is a bug.
- **Diagnostics' wording**: messages improve. Tools read `--diagnostics json`
  rather than the text.
- **The generated C, the runtime's C API and the export header's layout**:
  C built from one compiler's output links with that compiler's runtime.
  A C host rebuilds against a new release, and a Python package made by
  `scripts/gust_python.py` is rebuilt with it.
- **What `gust cimport` writes**: regenerate a package after upgrading.
- **Soundness fixes**: a program the compiler accepted only because of a
  soundness bug may stop compiling. Each such change is listed in the
  release notes with the rewrite it needs (`docs/gust/soundness_rust_164250.md`
  is the model).

## Editions

A change that would stop correct programs compiling (a new keyword taken
from identifiers, a rule made stricter, a default changed) waits for an
**edition**. An edition is a named set of such changes, `2027` say, that a
program opts into in its sidecar:

```goose
// program.build
edition = "2027";
```

- A program without an `edition` line keeps the edition it was written for:
  the first one, `2026`.
- Packages choose their own editions, and programs and packages of different
  editions build together: an edition changes how source is read, never what
  crosses between modules.
- Each edition comes with a migration note and, where it can be mechanical,
  a `gust format` rewrite.
- Editions are rare: at most one a year, and only for changes that earn it.

Additions that no existing program can meet, such as new syntax built from
characters that were errors before (`outer: for`, `break :outer`), new
standard library functions and new commands, need no edition and arrive in
any release.

## Versions

Gust's own versions follow semantic versioning from 1.0: a minor release adds
things, a patch release fixes them, and a major release is reserved for what
an edition cannot express (none is planned). `gust version` names the
compiler and runtime versions; `docs/gust/` keeps the design notes behind
each change, and `MODIFICATIONS.md` the list of what differs from Goose.

## Deprecation

Something on its way out first warns for at least one minor release, with
the replacement in the warning, and is removed only in a new edition.
