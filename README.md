# fs

Filesystem helpers extracted from [Xapiand](https://github.com/Kronuz/Xapiand):
recursive `mkdirs`, existence checks, glob-based delete/quarantine, directory
copy/move, and a pure-string path normalizer.

```cpp
#include "fs.hh"

mkdirs("/var/data/shard/3");              // create the whole chain
if (exists("/var/data/shard/3")) { ... }
delete_files("/var/data/shard/3", {"*.tmp", "*.lock"});  // glob delete; rmdir if emptied
copy_file("/src/dir", "/dst/dir");        // recursive directory copy
auto clean = normalize_path("a/b/../c");  // "a/c" (collapses . and .. and // )
```

## What's in it

| Function | Does |
| --- | --- |
| `normalize_path(p, slashed, keep_slash)` | pure-string collapse of `.` / `..` / repeated `/` |
| `exists(p)` / `mkdir(p)` / `mkdirs(p)` | stat / single dir / recursive dir chain |
| `delete_files(p, patterns)` | `fnmatch`-delete matching files; `rmdir` the dir if it ends up empty |
| `quarantine_files(p, patterns, suffix)` | rename matching files aside (into a `mkstemp` suffix) instead of deleting |
| `move_files(src, dst)` / `copy_file(...)` | move / recursively copy a directory tree |
| `opendir(p, create)` / `find_file_dir(...)` | directory iteration helpers |
| `build_path_index(p)` | create the index directory chain for a path |

## Dependencies

Uses a few Kronuz libraries for the real work: **io** (EINTR-safe file ops),
**strings** (`startswith`/`endswith`), **split** (path splitting), **stringified**
(`string_view` → NUL-terminated). Logging is an **optional** seam: `fs.cc`'s `L_*`
family is no-op by default (`fs_trace.h`), and the `error::` / `repr` helpers it would
log appear only inside those macros' arguments, so a host only needs them if it opts
into real logging via `FS_TRACE_HEADER`.

## Build / test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build        # unit test + self-checking Xapiand-style db layout example
```

## Use (FetchContent)

```cmake
FetchContent_Declare(fs GIT_REPOSITORY https://github.com/Kronuz/fs.git GIT_TAG main)
FetchContent_MakeAvailable(fs)
target_link_libraries(your_app PRIVATE fs::fs)
```

To route logging to your own logger, point `FS_TRACE_HEADER` at a header defining the
`L_*` family (and whatever `error::` / `repr` they reference).
