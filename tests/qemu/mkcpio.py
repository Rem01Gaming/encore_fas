#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a newc cpio archive: mkcpio.py out.cpio init fas_ctl encore_fas.ko"""
import os
import stat
import sys


def header(ino, name, mode, size):
    fields = (ino, mode, 0, 0, 1, 0, size, 0, 0, 0, 0, len(name) + 1, 0)
    raw = ("070701" + "".join("%08x" % v for v in fields)).encode()
    raw += name.encode() + b"\0"
    return raw + b"\0" * (-len(raw) % 4)


def main():
    out, files = sys.argv[1], sys.argv[2:]
    entries = [(d, stat.S_IFDIR | 0o755, b"") for d in ("dev", "proc", "sys", "tmp")]
    for f in files:
        mode = 0o755 if os.access(f, os.X_OK) else 0o644
        with open(f, "rb") as fh:
            entries.append((os.path.basename(f), stat.S_IFREG | mode, fh.read()))
    blob = bytearray()
    for ino, (name, mode, data) in enumerate(entries, 1):
        blob += header(ino, name, mode, len(data)) + data + b"\0" * (-len(data) % 4)
    blob += header(0, "TRAILER!!!", 0, 0)
    with open(out, "wb") as fh:
        fh.write(blob)


main()
