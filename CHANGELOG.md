# Changelog

All notable changes to `fs` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-11-01

### Fixed

- `normalize_path("")` returned `"/"` (with `keep_slash` off): the loop appended its closing
  `/` to an empty path, which is the root. An empty path names nothing (`mkdirs("")` is
  `false` for the same reason), so it stays empty in all four flag combinations, and a
  default-constructed `std::string_view` gives the same. Oracle: `std::filesystem::path`
  `lexically_normal`, which maps `""` to `""`; Python's `os.path.normpath` maps it to `"."`,
  which no caller wants. The in-family callers agree: `storage` appends volume names to
  `normalize_path(base, true)`, so `"/"` made `Storage("")` open `"/vol.0"` at the root of
  the filesystem, and `url-parser` passes `keep_slash`, where the result was already `""`.
  The README and the API table said `/`, and now say that an empty path stays empty. Every
  non-empty input gives the bytes it gave before: `test_normalize_path_frozen` compares the
  old loop with the new over every string of `a`, `.` and `/` up to length 8 and 30,000
  random ones in all four flag combinations, and checks that a relative path never becomes
  absolute. `test_normalize_path_empty` states the one class that changed (old `"/"`, new
  `""`) and fails with the old `fs.cc`.
- `normalize_path` never popped the first component of a relative path, because the pop
  loop stopped one byte above the start of the output to protect the root: `a/..` was `a`,
  `a/../b` was `a/b`, and `a/b/../..` was `a`. A `..` now pops it like any
  other component, and when nothing is left the path is `.` (`./` with `slashed` or a kept
  trailing `/`). The root is not popped, a leading `.` or `..` stays the `.` the output
  starts with (`../a` is still `./a`, `./a/..` is `.`), and a path with an empty result no
  longer writes a `/` at the start of the output. Oracle: `std::filesystem::path`
  `lexically_normal`, which gives `.` for `a/..` and `b` for `a/../b`, with the library's
  trailing-slash policy applied; it is used where it and the library agree (names and exactly
  `..`, no leading `..`, and with `keep_slash` no unterminated `..`). Everything else is
  unchanged: `test_normalize_path_frozen` lists the changed class with a model of it, requires
  the frozen loop's bytes outside it and the oracle's inside it, and checks that the frozen
  loop was wrong on some of what the oracle reaches, over every string
  of `a`, `.` and `/` up to length 8 and 30,000 random ones in all four flag combinations.
  `test_normalize_path_first_component` states the cases. The same fix is in `storage`.
  `url-parser` carries another copy of the loop, with the same bug.
- With `keep_slash` the last component is left as written, so `a/..` stays `a/..`; the
  README says so (it is not changed here).

### Tests

- `copy_file`'s same-file check is a file-local predicate, `same_file`, that
  compares the device and the inode number. `test/identity_test.cc` includes
  `fs.cc` and checks it on hand-made `stat` results, because the real
  filesystems of a build host rarely hand out one inode number on two devices.
  A comparison on the inode alone, which refuses an unrelated file on another
  device that carries the same number, survived `test/test.cc` on macOS and
  Linux; it fails `fs_identity`.
- `test_copy_file_across_filesystems` (`test/test.cc`) copies between
  directories on different devices (the working directory, `/tmp`,
  `/var/tmp`, `/dev/shm`, `$TMPDIR`): over an existing file, back again, as a
  directory copy, and with a symlink on the other device that points at the
  source (refused, source intact). It reports when two files on different
  devices carry the same inode number, and skips when the host has one
  filesystem.

## 2026-10-31

### Fixed

- `normalize_path` crashed on a default-constructed `std::string_view` (a null
  data pointer) with `keep_slash`: the loop bound `end - 1` wrapped around, so
  the loop ran from the null pointer through the address space. The loop now
  counts the bytes of `[src, end)` and then the closing `/`, with no pointer
  before the range, and a null view gives the result of `""`. The output for
  every non-empty input is unchanged (compared with a frozen copy of the old
  loop over every string of `a`, `.` and `/` up to length 8 and 30,000 random
  ones, in all four flag combinations).
- `mkdirs("")` read `front()` of an empty `string_view`, which is undefined
  behavior: an abort under `_GLIBCXX_ASSERTIONS`, a crash for a
  default-constructed view, and `true` otherwise. An empty path names no
  directory (`mkdir -p ""` fails with `ENOENT`), so `mkdirs` and
  `build_path_index("")` now return `false` and create nothing.
  `build_path_index("/name")`, a file in the root, reached the same call with
  `substr(0, 0)`; its parent is now `/`, which exists, so it still returns
  `true`.
- `copy_file` emptied its source when the source and the destination were one
  file (a directory copied onto itself, `copy_file(d, d, false, "f", "f")`, a
  destination that is a hard link or symlink to the source, another spelling of
  the path, a case-insensitive name): the destination was opened with `O_TRUNC`
  before the source was read, so the open truncated the source and the call
  returned `0` having copied nothing. The destination is now opened without
  `O_TRUNC`, `fstat` compares the device and inode of the two open descriptors,
  and a match returns `-1` before anything is written, as `cp` reports
  "identical (not copied)". Only then is a regular destination truncated
  (`ftruncate`; a FIFO or a device ignored `O_TRUNC` and is left alone). A
  directory copy stops at the first failure, so a same-file match in the middle
  of a directory leaves the files after it uncopied.
- The README described `copy_file` as copying a directory tree. It copies the
  regular files of one directory and does not enter subdirectories.
- `copy_file` treated the count `io::write` returns after a partial write as
  success, so a write that ran out of room mid-chunk (full disk,
  `RLIMIT_FSIZE`) left a truncated copy and returned `0`. It also ignored the
  destination's `close` result and relied on io's process-wide `EINTR` flag. It
  now writes the whole chunk (a partial count is followed by another write,
  which fails with the error), retries `EINTR` on read and write itself, and
  returns `-1` when the destination's `close` fails. The partial destination
  file is left in place, as for any other failure. Fault-injection tests cover
  an `RLIMIT_FSIZE` of 6000 bytes against a 6100-byte source and a 1 MiB copy
  into a slow FIFO under a 2 ms `SIGALRM` timer.
- The fs test links against Threads, which FreeBSD needs for the
  fault-injection test's reader thread.

## 2026-10-30

### Fixed

- `copy_file` opened the destination without `O_TRUNC`, so a shorter copy over a
  longer file kept the old tail. The destination open check tested `src_fd`
  instead of `dst_fd`, so a failed open went on to write to descriptor -1. Every
  early error return leaked the open descriptors, the directory stream leaked on
  the stat/mkdir error paths, and the first error message lacked its directory
  argument.
