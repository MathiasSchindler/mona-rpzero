#!/usr/bin/env python3

import argparse
import os
import stat
from pathlib import Path


def to_hex8(x: int) -> bytes:
    return f"{x:08x}".encode("ascii")


def pad4(n: int) -> int:
    return (4 - (n & 3)) & 3


def write_newc_entry(out, name: str, data: bytes, mode: int):
    namesz = len(name) + 1
    filesize = len(data)

    header = b"".join(
        [
            b"070701",  # c_magic
            to_hex8(0),  # c_ino
            to_hex8(mode),
            to_hex8(0),  # c_uid
            to_hex8(0),  # c_gid
            to_hex8(1),  # c_nlink
            to_hex8(0),  # c_mtime
            to_hex8(filesize),
            to_hex8(0),  # c_devmajor
            to_hex8(0),  # c_devminor
            to_hex8(0),  # c_rdevmajor
            to_hex8(0),  # c_rdevminor
            to_hex8(namesz),
            to_hex8(0),  # c_check
        ]
    )

    assert len(header) == 110
    out.write(header)
    out.write(name.encode("utf-8") + b"\x00")
    out.write(b"\x00" * pad4(110 + namesz))

    out.write(data)
    out.write(b"\x00" * pad4(filesize))


def build_from_dir(root: Path, out_path: Path):
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()

        rel_dir = Path(dirpath).relative_to(root)
        if str(rel_dir) == ".":
            rel_dir = Path("")

        # Emit directory entries (except root)
        if str(rel_dir) != "":
            st_mode = stat.S_IFDIR | 0o755
            files.append((str(rel_dir), b"", st_mode))

        for fn in filenames:
            p = Path(dirpath) / fn
            rel = (rel_dir / fn).as_posix()
            data = p.read_bytes()
            st = p.stat()
            mode = stat.S_IFREG | (st.st_mode & 0o777)
            files.append((rel, data, mode))

    with out_path.open("wb") as out:
        for name, data, mode in files:
            write_newc_entry(out, name, data, mode)
        write_newc_entry(out, "TRAILER!!!", b"", stat.S_IFREG | 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="Directory to pack")
    ap.add_argument("--out", required=True, help="Output .cpio path")
    args = ap.parse_args()

    root = Path(args.root)
    outp = Path(args.out)
    if not root.is_dir():
        raise SystemExit(f"root is not a directory: {root}")

    outp.parent.mkdir(parents=True, exist_ok=True)
    build_from_dir(root, outp)


if __name__ == "__main__":
    main()
