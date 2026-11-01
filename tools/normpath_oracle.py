#!/usr/bin/env python3
"""The oracle behind the expected values of test/test.cc's normalize_path tests.

normalize_path is Python's os.path.normpath with two differences, both written out in `model` below:
a run of leading '/' is one root (POSIX lets normpath keep two), and the trailing '/' is the flags' policy
(`slashed` adds one, `keep_slash` keeps the input's, the root is always "/").

    tools/normpath_oracle.py                   print the digest of the exhaustive strings
    tools/normpath_oracle.py --check FILE      the digest and the table rows written in FILE (test/test.cc)
                                               equal what posixpath.normpath gives (exit 1 if not)
    tools/normpath_oracle.py --table FILE      print the table of FILE regenerated: input, default,
                                               slashed, keep_slash, as C++ initializers

The digest is FNV-1a (64 bits) over the results of every string of 'a', 'b', '.' and '/' of length 1 to 8
(87,380 strings), in the order test_normalize_path_frozen visits them (by length; within a length the
first character is the least significant digit of the counter, with the alphabet "ab./"), each in the
three flag combinations plain, slashed, keep_slash, every result followed by "\\n".
"""
import posixpath
import re
import sys

ALPHABET = "ab./"
MAX_LENGTH = 8
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = 0xFFFFFFFFFFFFFFFF


def model(s: str, slashed: bool, keep_slash: bool) -> str:
    p = posixpath.normpath(s)
    if p.startswith("//") and not p.startswith("///"):
        p = p[1:]  # one root
    if (s.endswith("/") if keep_slash else slashed) and not p.endswith("/"):
        p += "/"
    return p


def strings():
    for length in range(1, MAX_LENGTH + 1):
        for k in range(len(ALPHABET) ** length):
            chars = []
            for _ in range(length):
                chars.append(ALPHABET[k % len(ALPHABET)])
                k //= len(ALPHABET)
            yield "".join(chars)


def digest() -> tuple[int, int]:
    h = FNV_OFFSET
    count = 0
    for s in strings():
        count += 1
        for flags in range(3):
            for byte in (model(s, bool(flags & 1), bool(flags & 2)) + "\n").encode():
                h = ((h ^ byte) * FNV_PRIME) & MASK
    return h, count


def table_rows(text: str):
    """The rows {"in", "plain", "slashed", "kept"} of test_normalize_path_table in the test source."""
    start = text.index("static void test_normalize_path_table")
    body = text[start : text.index("for (const Row& row : rows)", start)]
    string = r'"((?:[^"\\]|\\.)*)"'
    row = re.compile(r"\{" + r",\s*".join([string] * 4) + r"\}")
    return [m.groups() for m in row.finditer(body)]


def check(path: str) -> int:
    text = open(path, encoding="utf-8").read()
    bad = 0
    h, count = digest()
    written = re.search(r"digest == (0x[0-9a-fA-F]+)ULL", text)
    if not written or int(written.group(1), 16) != h:
        print(f"digest: {path} has {written.group(1) if written else 'none'}, the oracle gives {h:#018x} over {count} strings")
        bad += 1
    else:
        print(f"digest {h:#018x} over {count} strings ({count * 3} results): ok")
    rows = table_rows(text)
    for s, plain, slashed, kept in rows:
        want = (model(s, False, False), model(s, True, False), model(s, False, True))
        if (plain, slashed, kept) != want:
            print(f"table: {s!r}: {path} has {(plain, slashed, kept)}, the oracle gives {want}")
            bad += 1
    print(f"table: {len(rows)} rows, {bad} wrong" if rows else "table: no rows found")
    return 1 if bad or not rows else 0


def table(path: str) -> int:
    for s, *_ in table_rows(open(path, encoding="utf-8").read()):
        cells = [s, model(s, False, False), model(s, True, False), model(s, False, True)]
        print("\t\t{" + ", ".join('"' + c + '"' for c in cells) + "},")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--check":
        return check(argv[2])
    if len(argv) == 3 and argv[1] == "--table":
        return table(argv[2])
    if len(argv) == 1:
        h, count = digest()
        print(f"{h:#018x}  {count} strings, {count * 3} results")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
