# Changelog

All notable changes to `fs` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31

### Fixed

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
