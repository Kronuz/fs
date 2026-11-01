# fs

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/fs/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/fs/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

POSIX filesystem helpers for C++20: mkdir -p, glob delete and quarantine, directory copy and move, path normalization.
They are free functions over POSIX directory calls, with file I/O through
[io](https://github.com/Kronuz/io) so a copy survives signals. Reach for them in
server code that manages its own data directories.

## ✨ Highlights

- **The whole chain.** `mkdirs("a/b/c")` creates every missing directory, like
  `mkdir -p`.
- **Glob delete that cleans up.** `delete_files` removes the regular files that
  match any `fnmatch` pattern, then the directory once nothing is left in it.
- **Recoverable delete.** `quarantine_files` moves matching files into a fresh
  `.quarantine.XXXXXX` directory instead of deleting them.
- **Careful copies.** `copy_file` reports a short write as `-1`, retries `EINTR`,
  and refuses to copy a file onto itself (same device and inode) before it
  truncates anything.
- **Pure path normalization.** `normalize_path` collapses `.`, `..`, and
  repeated `/` without touching the disk.
- **Optional logging.** Diagnostics go through `FS_TRACE_HEADER`, a no-op unless
  the host supplies one.

## 🚀 Quick start

```cpp
#include "fs.hh"

#include <cstdio>
#include <fstream>

int main() {
	mkdirs("demo/shard/3");  // the whole chain, like mkdir -p
	std::ofstream("demo/shard/3/a.tmp") << "x";
	std::ofstream("demo/shard/3/b.lock") << "x";
	delete_files("demo/shard/3", {"*.tmp", "*.lock"});  // then rmdir, since it ends up empty
	std::printf("%d\n", exists("demo/shard/3"));  // 0
	std::printf("%d\n", exists("demo/shard"));  // 1
	std::printf("%s\n", normalize_path("a/./b/../c//d").c_str());  // a/c/d, without touching the disk
}
```

[`examples/db_layout.cc`](examples/db_layout.cc) builds a self-checking,
Xapiand-style database directory layout.

## 📦 Installation

Requires a C++20 compiler, a POSIX system, and CMake 3.16 or newer. It fetches
[io](https://github.com/Kronuz/io), [strings](https://github.com/Kronuz/strings),
[split](https://github.com/Kronuz/split), and
[stringified](https://github.com/Kronuz/stringified), pinned by commit. CI builds
it on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan,
UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(fs
  GIT_REPOSITORY https://github.com/Kronuz/fs.git
  GIT_TAG        c4bf223d9ba08361c71a863c2e9a73bb50fdefcf)  # or main for the tip
FetchContent_MakeAvailable(fs)

target_link_libraries(my_app PRIVATE fs::fs)
```

```cpp
#include "fs.hh"
```

`fs::fs` is a static library built from `fs.cc`.

## 🎯 When to use it

Use it in POSIX server code that lays out its own data directories: creating
shard trees, clearing or quarantining files by pattern, and receiving files into
a temporary directory before moving them into place.

Not for:

- **Portable path work.** `std::filesystem` covers most of this on every
  platform.
- **Recursive trees.** The directory helpers act on the regular files of one
  directory and never enter subdirectories.
- **Error details from bulk helpers.** `delete_files`, `quarantine_files`, and
  `move_files` return nothing; a failure reaches only the logging seam.
- **Filesystems that report `DT_UNKNOWN`.** Regular files are picked by the
  `d_type` that `readdir` returns, so entries without one are skipped.
- **`keep_slash` on a trailing `.` or `..`.** With `keep_slash` the last component
  is left as written (`a/..` stays `a/..`); a component is resolved only when a
  `/` follows it.

## 📚 Documentation

- [docs/API.md](docs/API.md): every function, its return value, and the
  `normalize_path` flags.
- [ARCHITECTURE.md](ARCHITECTURE.md): dependencies, the logging seam, and the
  behaviors callers rely on.
- [CHANGELOG.md](CHANGELOG.md): what changed.
- [AGENTS.md](AGENTS.md): repository map and invariants.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#fs)
  in Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo (Kronuz) and Copyright (c) 2015-2020
Dubalu LLC. See [LICENSE](LICENSE). Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand).
