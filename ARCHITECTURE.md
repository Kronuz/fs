# Architecture

`fs` is a flat set of free functions over POSIX directory calls. There is no state.
The value is in the recursion, glob matching, and path-string normalization; the raw
byte I/O is delegated to the `io` library.

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
