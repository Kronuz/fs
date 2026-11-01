# Changelog

All notable changes to `fs` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
