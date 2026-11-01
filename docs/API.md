# fs API

Every function is declared in `fs.hh`, lives in the global namespace, and takes
`std::string_view` paths. Directory helpers act on the regular files of one
directory and do not enter subdirectories; they pick regular files by the
`d_type` that `readdir` reports.

| Function | Does |
| --- | --- |
| `normalize_path(p, slashed, keep_slash)` | collapses `.`, `..`, and repeated `/` as a pure string operation |
| `exists(p)` | `true` when `stat` succeeds |
| `mkdir(p)` / `mkdirs(p)` | one directory / the whole chain, like `mkdir -p` |
| `build_path_index(p)` | `mkdirs` the parent directory of `p` |
| `delete_files(p, patterns)` | deletes the regular files matching any `fnmatch` pattern, then `rmdir`s `p` if nothing is left in it |
| `quarantine_files(p, patterns, suffix)` | moves the matching regular files into a fresh `mkdtemp` directory, `<p>/.quarantine.XXXXXX` by default |
| `move_files(src, dst)` | renames the regular files of `src` into `dst`, then `rmdir`s `src` |
| `copy_file(src, dst, create, file_name, new_name)` | copies the regular files of `src` (or only `file_name`, stored as `new_name`) into `dst`, creating `dst` when `create` is set |
| `opendir(p, create)` | `::opendir`, creating the directory first when `create` is set |
| `find_file_dir(dir, fptr, pattern, pre_suf_fix)` | advances `fptr` to the next regular file whose name starts with `pattern` (`pre_suf_fix` true) or ends with it |

## Return values and errors

`mkdir`, `mkdirs`, and `build_path_index` return `false` on failure; `mkdirs` and
`build_path_index` also return `false` for an empty path, as `mkdir -p ""` fails.
`delete_files`, `quarantine_files`, and `move_files` return nothing: a failure
goes to the logging seam (`FS_TRACE_HEADER`, silent by default) and the helper
moves on to the next file.

`copy_file` returns `0` only when every byte of every copied file was written and
the destination closed cleanly. A failed or short write (a full disk,
`RLIMIT_FSIZE`, a quota reported at `close`) returns `-1`, logs the error, and
leaves the partial destination file in place, as for any other failure. A signal
(`EINTR`) is retried. A directory copy stops at the first failure.

`copy_file` refuses to copy a file onto itself. When the source and the
destination are one file (the same device and inode, reached by the same path,
another spelling of it, a symlink, a hard link, or a case-insensitive name) it
returns `-1`, as `cp` reports "identical (not copied)", and the file is left as
it was. The check reads the identity from the open descriptors, before the
destination is truncated.

## Path normalization

`normalize_path` never touches the filesystem, so it works on paths that do not
exist and does not resolve symlinks. By default it drops a trailing `/`;
`slashed` adds one, and `keep_slash` leaves the input's trailing `/` as it is.

| input | `normalize_path(p)` | `slashed` | `keep_slash` |
| --- | --- | --- | --- |
| `a/b/` | `a/b` | `a/b/` | `a/b/` |
| `/x/../y/./z//` | `/y/z` | `/y/z/` | `/y/z/` |
| `../a` | `./a` | `./a/` | `./a` |
| (empty) | (empty) | (empty) | (empty) |

A leading `..` becomes `.`. An empty path names nothing, so it stays empty in
every combination (it is not the root, and `slashed` adds no `/` to it); a name
appended to the result stays relative.
