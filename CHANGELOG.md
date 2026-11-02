# Changelog

All notable changes to `fs` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-11-01

### Changed

- `normalize_path` runs in one pass and is faster than at the campaign's start (ce16118) on 12 of the 14
  comparable rows of the bench on the M4 (one ties, `np_relative` is 0.92x) and on all 14 on the i9,
  after the three correctness rewrites (`bf10f39`, `7a10a04`, `d172a13`) had made it 0.69x to 0.84x of
  START on the M4 and 0.45x to 0.88x on the i9 (the gate; 1116 ns against 502 ns for a 40-name path on the
  i9). Each name is copied to the output as it is read and kept or dropped at its `/` (no second walk over
  the name, one test for a name that is not plain, and a `..` that pops no longer scans back over a `..`
  prefix: `floor` marks its end), and the
  string overload builds a path of up to 510 bytes on the stack and copies it once into a string of its
  size, where it was resized to `size + 2` (a zero fill) and shrunk, which with libstdc++ allocated for any
  path of 14 bytes or more. The bytes are the same as the loop of 8934dbc on every input:
  `test/normalize_test.cc` (CTest `fs_normalize`) compares them in all four flag combinations over every
  string of `/`, `.`, `a`, `b` up to length 10 and of `/`, `.`, `a` up to length 12, 800,000 random strings
  of names (1 to 40 bytes, dots, runs of slashes) and of bytes (NUL, 0x2d to 0x30, 0x80 to 0xff), and every
  path length from 0 to 600 through the string overload, on exact-size heap blocks, so an AddressSanitizer
  build fails on a byte read past the input or written past the `(end - src) + 2` bytes the contract gives
  (12 million comparisons; ASan, UBSan and TSan green). Ten mutants of the new code (the pop floor is the
  base, the dot test's mask is `0xFF`, no separator, a popped name leaves its `/`, no floor after a kept
  `..`, a `.` kept, the root keeps a `..`, a stack buffer two bytes short, no closing `/`, an empty path is
  not `.`) are each caught by it. `bench/normalize_path_bench.cc` (`-DFS_BUILD_BENCH=ON`) builds the
  library's loop, the loop of 8934dbc and the one of ce16118, each with its string wrapper. ns per call, on
  the M4 Pro (arm64; START and now from a hold of 3 paired runs with A/A, 8934dbc from an earlier one of 5
  of the same code) and the i9 (x86-64, 5 paired runs):

  | row | M4 START | M4 8934dbc | M4 now | M4 START/now | i9 START | i9 8934dbc | i9 now | i9 START/now |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
  | np_plain | 9.30 | 11.6 | 8.30 | 1.12x | 29.9 | 30.0 | 19.7 | 1.52x |
  | np_relative | 8.50 | 11.0 | 9.10 | 0.92x | 27.2 | 29.1 | 20.4 | 1.33x |
  | np_dots | 22.8 | 20.2 | 14.3 | 1.58x | 57.0 | 46.5 | 34.4 | 1.66x |
  | np_trailing_slash | 13.4 | 14.7 | 12.3 | 1.10x | 38.6 | 32.8 | 21.6 | 1.78x |
  | np_slashed | 13.4 | 14.5 | 11.9 | 1.13x | 37.4 | 32.5 | 21.0 | 1.77x |
  | np_repeated_slashes | 16.0 | 17.2 | 13.2 | 1.21x | 45.9 | 37.4 | 24.0 | 1.94x |
  | np_keep_slash | 12.3 | 14.9 | 12.3 | 1.01x | 34.5 | 32.9 | 20.9 | 1.65x |
  | np_xapiand_path | 45.4 | 52.1 | 36.4 | 1.22x | 125.7 | 108.9 | 79.5 | 1.58x |
  | np_long | 292.8 | 352.1 | 200.4 | 1.46x | 725.0 | 575.9 | 396.0 | 1.83x |
  | np_long_dots | 262.2 | 251.0 | 159.4 | 1.65x | 713.5 | 598.4 | 373.2 | 1.91x |
  | np_dot | 5.70 | 5.50 | 4.40 | 1.30x | 17.3 | 13.9 | 12.0 | 1.44x |
  | np_root | 5.40 | 5.20 | 4.10 | 1.29x | 16.6 | 13.0 | 10.9 | 1.54x |
  | np_abs_dotdot_end | 12.2 | 11.8 | 9.00 | 1.33x | 36.4 | 30.3 | 22.1 | 1.64x |
  | np_abs_dotdot_root | 13.7 | 13.0 | 9.60 | 1.43x | 40.0 | 29.6 | 22.3 | 1.79x |

  Geomean of START/now: 1.25 on the M4, 1.66 on the i9. Against 8934dbc every row is faster on both
  (1.21x to 1.76x on the M4, 1.17x to 1.62x on the i9). `np_relative` (`a/b/c/d`, four one-byte names) is
  0.92x of START on the M4 in this bench (0.6 ns of 9) and `np_keep_slash` ties; the gate's micro-bench
  (each result folded into an FNV-1a in the timed loop) has them at 1.05x and 1.06x of START on the M4 and
  every row at or above START on both machines (M4 geomean 1.39, i9 1.45). The leading-`./` and `../` rows
  and the three rows whose result differs from START's by design (an empty path, a `..` that pops the
  first component) are listed apart in the bench. What was tried for `np_relative` (a first byte copied
  without a test, a loop that carries each name's closing `/`, the result written into the string instead
  of the stack buffer: none moved it more than the layout noise, and the first made a 40-name path 1.5x
  slower on the i9) and the next step are in issue #1.
- `tools/normpath_oracle.py` regenerates the expected values of `test/test.cc`: the digest
  `0xef8fe9971d37494b` over the 87,380 strings (the script that produced it was lost) and the 55-row table,
  from `posixpath.normpath` with one root for a run of leading `/` and the flags' trailing `/` policy.
  `--check test/test.cc` (CTest `fs_normpath_oracle`, when `python3` is found) fails when either differs.

### Fixed

- `normalize_path` returns the lexical normal form of Python's `os.path.normpath`, and the
  current directory has one spelling, `.`. Before, the empty path was `"/"` (the closing `/`
  appended to nothing, the root), `.` with `keep_slash` was `""`, and `x/..` was `x`: the pop
  loop never removed the first component of a relative path (`a/../b` was `a/b`, `aaa/..` was
  `a`). A normalizer that answers `""` for one spelling of the current directory and `.` for
  another gives callers two paths to compare, and `""` is not a path that `open` or `stat`
  accepts. `""`, `.`, `./`, `x/..`, `x/../`, `./x/..` are now `.` (`./` with `slashed`, and
  with `keep_slash` for a spelling that ends in `/`). A `..` was also one dot short (`..` was
  `.`, `../a` was `./a`), and `...` popped two components. Now a `..` removes the component
  before it, the first included; a relative path keeps the `..` it cannot remove (`../x` stays
  `../x`, `a/../..` is `..`); the root absorbs it (`/x/..` and `/..` are `/`); a leading `./`
  is dropped (`./a` is `a`); a component of three or more dots is a name. `keep_slash` keeps
  the input's trailing `/` and resolves every component, the last one included (`a/..` is
  `.`, `a/../` is `./`). A run of leading `/` is one root (`//x` is `/x`; `os.path.normpath`
  keeps two). A default-constructed `std::string_view` is the empty path. Oracle:
  `os.path.normpath`, not `std::filesystem::path::lexically_normal`, which maps `""` to `""`
  and `x/..` to `.`, the two spellings the ruling unifies. `test_normalize_path_table` is
  `os.path.normpath`'s output for 55 inputs in three flag combinations;
  `test_normalize_path_frozen` checks a stack-of-components model against the library over
  every string of `a`, `b`, `.` and `/` up to length 8 and 30,000 random ones in all four flag
  combinations, that a second normalization changes nothing, that the results over the 87,380
  exhaustive strings hash to the digest of Python's own results, and that every non-empty
  input without a `.` gives the bytes the old loop gave. With the old `fs.cc` the table, the
  digest and the empty-path test fail. `storage` carries the same function. `url-parser`
  carries a third copy of the old loop, which has the same bugs.

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
  before the range, and a null view gives the result of the empty path. The output for
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
