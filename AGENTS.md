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

## Verification and benchmarks

The shared method is the catalog's
[PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md). From a
fresh clone (configure fetches `io`, `strings`, `split` and `stringified` at the commits
`CMakeLists.txt` pins):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure
```

- **`fs`** (`test/test.cc`, prints `all fs tests passed`). `normalize_path`'s oracle is Python's
  `os.path.normpath`: 55 inputs in three flag combinations with normpath's output written in
  (`test_normalize_path_table`; check a row with `python3 -c 'import posixpath; print(posixpath.normpath("a/../b"))'`),
  and every string of `a`, `b`, `.` and `/` up to length 8 (87,380) plus 30,000 random ones in all
  four flag combinations, against a stack-of-components model, a second pass, the digest of
  normpath's own results (the script that produced it is not in the repository), and a frozen
  copy of the old loop on inputs without a `.`. Then a mkdirs, delete and exists round trip,
  `copy_file` onto itself and across filesystems, and its faults (an `RLIMIT_FSIZE` short write, a
  slow FIFO under a 2 ms `SIGALRM`).
- **`fs_identity`** (`test/identity_test.cc`) checks the (device, inode) identity `copy_file`
  refuses on, on hand-made `stat` results; an inode-only comparison fails it. **`fs_db_layout`**
  (`examples/db_layout.cc`) builds Xapiand's database layout and checks itself.
- **Revert checks.** With the `fs.cc` from before the 2026-11-01 `normalize_path` fixes
  (`git show 7a10a04^:fs.cc > fs.cc`, rebuild, run `build/fs_test`, then `git checkout fs.cc`),
  the table, the digest and the empty-path checks fail.
- **CI.** `.github/workflows/ci.yml` calls the catalog's shared
  [`cmake.yml`](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml):
  Linux x86_64 (GCC, Clang) and arm64 (GCC), macOS arm64 and x86_64, and FreeBSD, at `-O2 -g` with
  the standard library's assertions on, plus ASan+UBSan and TSan (the fault test runs a reader
  thread). The ASan+UBSan job, on Linux:

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  ```

- **Not here.** No mutation script, fuzz target or benchmark, and no performance claim.

## Host integration (how Xapiand uses it)

Xapiand compiles `fs.cc` into its own object list with
`-D FS_TRACE_HEADER=<xapiand_fs_trace.h>`, so fs logging lands in Xapiand's logger; the
header comes from this repo's include dir via FetchContent. The simple path for a host
that doesn't need logging is to link the static `fs::fs` target (no-op logging).
