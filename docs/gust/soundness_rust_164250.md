# rust-lang/rust#164250 and Gust

Checked on 2026-10-25. The issue: [struct's lifetime requirements can be
bypassed via covariant field assignment](https://github.com/rust-lang/rust/issues/164250).

## Summary

- **The Rust bug is open and unfixed.** It is labelled `I-unsound` and
  `P-high`; no pull request fixes it yet. The reports in the issue show it
  in every Rust from 1.4.0 (2015) to stable 1.99.0 and nightly 1.101.0. On
  this machine it reproduces on stable 1.96.0 and nightly 1.94.0 (below). No
  Rust version is safe from it, so updating Rust would not help.
- **It does not touch Gust's build.** The compiler, runtime, tests and CI are
  C++, C and Python; Rust appears only in `bench/rust/`, the comparison
  programs `bench/run_bench.py` times against Gust, and none of them has a
  `Drop` implementation or a lifetime bound.
- **Gust's language has no equivalent.** The bug needs three things Gust's
  design leaves out, and the check that guards its references is applied to
  every store rather than to a type. Twenty-eight programs that try to
  reproduce it through every channel we could find are all rejected; the
  sixteen closest to the issue are now regression tests, beside a test of
  the valid stores that must keep compiling.
- **One requirement for later.** If Gust adds destructors (upstream's
  `docs/design/scope_exit_cleanup.md` studies them), destruction order
  within a scope becomes part of the store rule. That is the gap Rust's
  drop check exists for.

## The Rust bug

```rust
struct Q<T: 'static>(Option<T>);
impl<T: 'static> Drop for Q<T> {
    fn drop(&mut self) { /* trusts that T is 'static */ }
}
fn main() {
    let l = String::new();
    let mut q = Q(None);
    q.0 = Some(&l);   // accepted: Q's T becomes &'short String
}
```

Three ingredients:

1. **A bound on a type's lifetime parameter.** `Q<T: 'static>` promises that
   anything stored in `Q` lives forever.
2. **Inference that shortens it unchecked.** `Option<T>` is covariant, so
   the assignment makes the compiler pick `T = &'short String` for `q`'s
   type, and the bound on `Q` is not re-checked against that choice.
3. **Code that runs later and relies on the bound.** `Drop::drop` runs at
   scope exit with the `'static` promise in hand, and can stash the
   reference somewhere that outlives `l`. Other reports in the issue use a
   closure stored in the struct, or `std::mem::swap` on an array whose
   element type was unified from two roots.

Reproduced here, from the issue's first report and from a comment's version
without `'static` (`me_out` ends up pointing at freed memory):

| toolchain | report 1 | report 2 |
|---|---|---|
| rustc 1.96.0 stable | compiles, runs | compiles, prints the reused memory `XXXXXXXXXXXXXXXX` |
| rustc 1.94.0-nightly | compiles, runs | compiles, prints `XXXXXXXXXXXXXXXX` |

## Why Gust has no equivalent

**1. No lifetime bounds to shorten.** Gust has no lifetime parameters at all.
A reference's lifetime is a *root*: the variable that owns its target,
inferred per value and never part of a type (`docs/reference.md` §12, spec
§9.2). A struct holding references, a *holder*, is "rooted at what was
stored into it". There is no `Q<'a>` whose `'a` inference could pick wrongly.

**2. Every store is checked, not every type.** The store rule lives in the
value-to-destination fit check, `src/typecheck_exprs.h` (search for
`which does not outlive the destination`): for a reference, a slice or a
holder being stored anywhere (variable, field, element, global, a field
being constructed), every place the value may point (`roots.alts`) must be
at least as shallow as every place the destination could be owned by
(`ShrinkTargets`). A callee that stores through a parameter is checked in
its specialization for the caller's roots (functions are specialized per
distinct root), so a valid first call cannot vouch for a later invalid one
(`covariant_store_specialized`).

**3. Nothing runs later.** Gust has no destructors, no `Drop` and no `defer`;
memory is freed by scope exit alone. Function values cannot be stored,
returned or put in data (`docs/reference.md` §18), which closes the
issue's stored-closure variant. Values cross between threads only when they
are flat, holding no reference at any depth (§22), which closes the
thread variants (`covariant_store_thread_arg`).

## What we tried

Every program below tries to make a reference outlive its target through
one channel; all were rejected by `gust check`. The ones marked with a
fixture name are now in `test/errors_tc/`.

| channel | result | fixture |
|---|---|---|
| a field of an outer holder | rejected, store rule | `covariant_store_field` |
| a whole holder literal | rejected, store rule | `covariant_store_whole_holder` |
| a callee storing through a parameter | rejected, store rule | `covariant_store_callee` |
| a generic setter `set<T>(q: G<T>&, x: T&)` | rejected, store rule | `covariant_store_generic` |
| `swap` of an outer and an inner holder | rejected, store rule | `covariant_store_swap` |
| one array unifying two roots, element copied out | rejected, store rule | `covariant_store_array_unify` |
| an element of an outer array of holders | rejected, store rule | |
| a push into an outer `(i64&)[>..]` | rejected, store rule | |
| a global holder | rejected, store rule | |
| a trailing block writing an enclosing holder | rejected, store rule | `covariant_store_block` |
| a nested function writing an enclosing holder | rejected, store rule | `covariant_store_nested_fn` |
| a function returning a holder of its local | rejected, return rule | |
| a dictionary value in an outer dictionary | rejected, store rule | `covariant_store_dictionary` |
| `return ... from` carrying a local | rejected, long-distance return rule | |
| `copy` of an inner holder into an outer one | rejected, store rule | `covariant_store_copy` |
| rebinding a reference variable with `.=` | rejected, store rule | |
| a holder inside a holder | rejected, store rule | `covariant_store_nested_holder` |
| a queue message holding a reference | rejected, flat-only queues | |
| a recursive function storing an activation's local | rejected, store rule | `covariant_store_recursive` |
| a variable-mode enum payload | rejected, store rule | `covariant_store_enum_payload` |
| one setter called with a valid root, then a short one | rejected, store rule | `covariant_store_specialized` |
| a branch merging an outer and an inner reference | rejected, merged-value rule | `covariant_store_branch` |
| a loop body's local | rejected, store rule | |
| a holder pointing into a grow-shrink array | rejected, grow-shrink rule | |
| a reference read out of a parameter's holder | rejected, store rule | |
| a worker argument holding a reference | rejected, flat-only arguments | `covariant_store_thread_arg` |
| a holder of a slice of an inner growable array | rejected, store rule | |
| a dictionary slot reference kept across an insert | rejected, shrink rule | |

`test/lifetimes/holder_stores_valid.goose` (with
`test/expected/holder_stores_valid.out`) pins down the other side: the same
channels with references that live long enough (a field, a callee, a
generic, `swap`, an array, `copy`, a nested holder, a block, a reused
specialization, and a holder declared before its target in one scope) all
compile, run and write through to their targets.

## The requirement this leaves for destructors

A holder may point at a variable declared after it in the same scope
(`holder_stores_valid`'s first case): the store rule compares scope depth,
and both die at the same exit, which frees them together. That is sound
because nothing runs at the exit. Upstream's research note on cleanup at
scope exit (`docs/design/scope_exit_cleanup.md`) weighs adding `defer` and
destructor types:

- **`defer`** is checked as if written at the end of its scope, where every
  local of the scope is still alive (§4.4 of the note). A `defer` body that
  reads a holder therefore sees live targets: no new requirement.
- **Destructor types** would run code per value at the exit, in some order.
  A destructor reading a holder's reference to a variable destroyed just
  before it would be the Rust bug's third ingredient. If Gust adds them, the
  store rule must also order variables within a scope: a value with a
  destructor may hold references only to variables declared before it, or
  to types that run no code on destruction. That is what Rust's drop check
  does, with its `may_dangle` escape. The regression tests above should
  then gain the same-scope cases.
