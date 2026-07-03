# Architecture

`fs` is a flat set of free functions over POSIX directory calls. There is no state.
The value is in the recursion, glob matching, and path-string normalization; the raw
byte I/O is delegated to the `io` library.

![fs: filesystem helpers delegating byte I/O to io, path work to string libs](assets/architecture.svg)

<!-- Diagram: assets/architecture.svg. Edit the D2 source below and re-render with:
     d2 --theme 0 --pad 20 <this-source>.d2 assets/architecture.svg

```d2
# fs: filesystem helpers delegating byte I/O to io, path work to string libs.
direction: down
api: "fs API\n(mkdirs, delete_files, copy_file, normalize_path, ...)" { style.fill: "#e8f5ee" }
deps: "dependencies" {
  style.fill: "#eef2f7"
  grid-columns: 4
  io: "io\n(EINTR-safe file ops)"; split: "split\n(path by '/')"; strings: "strings\n(prefix/suffix)"; stringified: "stringified\n(NUL-terminate)"
}
seam: "FS_TRACE_HEADER\n(L_* logging + error:: / repr; no-op default)" { style.fill: "#faf3e6" }
api -> deps.io; api -> deps.split; api -> deps.strings; api -> deps.stringified
api -> seam: "diagnostics" { style.stroke-dash: 3 }
```
-->

## Dependencies

Kronuz libraries, fetched at tip: **`io`** (EINTR-safe `open`/`close`/`read`/`write`/
`mkdtemp`), **`split`** (`Split<>` to walk a path by `/`), **`strings`**
(`startswith`/`endswith`), **`stringified`** (`string_view` → NUL-terminated). Logging
is **not** a dependency: the `L_*` family is no-op by default and its `error::`/`repr`
arguments are dropped unevaluated, so those enter only when a host opts in via
`FS_TRACE_HEADER`.

## Files

```
fs.hh        The API: free functions (global namespace, verbatim from Xapiand).
fs.cc        The implementations -- the only compiled unit; where logging is emitted.
fs_trace.h   No-op L_* logging defaults (redirect via FS_TRACE_HEADER).
```

## Dependencies and the logging seam

Real work is delegated to Kronuz libraries: **io** for EINTR-safe `open`/`close`/
`read`/`write`/`mkdtemp` (so a copy survives signals), **split** to walk a path by
`/`, **strings** for prefix/suffix tests, **stringified** to hand a `string_view` to a
C API as a NUL-terminated string.

Logging is the one host coupling, and it is an optional compile-time seam. `fs.cc`
includes `FS_TRACE_HEADER` if defined, else `fs_trace.h`, which defines `L_CALL` /
`L_DEBUG` / `L_ERR` / `L_INFO` / `L_WARNING` (and `L_FS`, an fs-specific verbose trace)
as no-ops. The error-string helpers (`error::name`/`description`) and `repr(path)` are
referenced **only inside** those macros' arguments, so a no-op expansion never
evaluates them -- which is why the standalone build needs no errno-names or repr
dependency. `L_FS` is `#ifndef`-guarded to `L_NOTHING`, so it stays off even when a
host wires up the rest of the family.

## Notable behaviors (verbatim from Xapiand)

- **`delete_files` self-cleans.** After deleting every file matching the patterns, if
  the directory has no remaining files or subdirectories it is `rmdir`'d. Callers rely
  on this (a shard directory disappears once its files are gone).
- **`quarantine_files`** renames matches aside into a `mkstemp`-suffixed name rather
  than removing them -- a recoverable "delete".
- **`normalize_path` is pure string manipulation**: it never touches the filesystem,
  so it works on non-existent / symlinked paths and is safe to call anywhere.

## Invariants

- **Verbatim behavior.** The logic is byte-for-byte from Xapiand's in-tree `fs.*`.
- **File I/O goes through `io`**, not raw syscalls, so EINTR / short-count handling
  stays consistent with the rest of the stack.
- **No new hard dependencies.** Logging/error/repr stay behind the trace seam.
