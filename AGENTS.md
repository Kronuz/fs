# AGENTS

Orientation for anyone (human or agent) working on this library.

## What this is

Filesystem helpers extracted verbatim from Xapiand: recursive mkdir, glob
delete/quarantine, directory copy/move, and a pure path normalizer. One compiled
unit (`fs.cc`) plus a header (`fs.hh`). Read `ARCHITECTURE.md` first.

## File map

```
fs.hh         The API (free functions, global namespace).
fs.cc         The implementations -- the only compiled unit, where logging is emitted.
fs_trace.h    No-op L_* logging defaults (redirect via FS_TRACE_HEADER).
test/test.cc  ctest: normalize_path cases + a real mkdirs/delete/exists round-trip, copy_file same-file, cross-filesystem and fault cases.
test/identity_test.cc  ctest: the (device, inode) identity copy_file refuses on, on hand-made stat results (includes fs.cc).
```

## Dependencies (Kronuz family, via FetchContent at tip)

`io` (EINTR-safe file ops), `strings` (startswith/endswith), `split` (Split<>),
`stringified` (string_view -> NUL-terminated). Logging is NOT a dependency: the L_*
family is no-op by default, and `error::`/`repr` appear only inside L_* arguments, so
they are pulled in only when a host opts into real logging via `FS_TRACE_HEADER`.

## Conventions / invariants

- **Behavior from Xapiand.** The code comes from Xapiand's in-tree `fs.*`. Do not change
  the glob semantics or the `delete_files` empty-dir `rmdir`. `normalize_path` is
  `os.path.normpath`'s normal form (test/test.cc checks it against a model, a digest of
  Python's own output over 87,380 strings, and a frozen copy of the old loop on inputs
  with no `.`); the current directory is `.` in every spelling, the empty path included.
- **File I/O goes through `io::`**, never raw syscalls -- keep EINTR/short-count
  handling consistent.
- **One seam only: `FS_TRACE_HEADER`.** `error::`/`repr`/`L_*` may appear only inside
  L_* macro arguments; referencing one outside adds a hard dependency -- don't.
- **Functions are in the global namespace** (verbatim). `mkdir`/`exists` shadow nothing
  by signature but are a known namespace-pollution wart; namespacing them is a
  deliberate future change that touches every caller, not a drive-by.

## Build / test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
```

## Host integration (how Xapiand uses it)

Xapiand compiles `fs.cc` into its own object list with
`-D FS_TRACE_HEADER=<xapiand_fs_trace.h>`, so fs logging lands in Xapiand's logger; the
header comes from this repo's include dir via FetchContent. The simple path for a host
that doesn't need logging is to link the static `fs::fs` target (no-op logging).
