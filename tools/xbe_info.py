#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read an Xbox executable's sections and libraries, or an address in it.

    python tools/xbe_info.py DOA2.xbe
"""
import hashlib
import struct
import sys


def load(data):
    """(base, [(virtual address, bytes on the disc, file offset, name)]) of an XBE's sections."""
    base, = struct.unpack_from("<I", data, 0x104)
    count, address = struct.unpack_from("<II", data, 0x11C)
    sections = []
    for i in range(count):
        _flags, virtual, size, offset, on_disc, name_at = struct.unpack_from(
            "<IIIIII", data, address - base + 56 * i)
        name = ""
        if name_at:
            at = name_at - base
            end = data.find(b"\0", at)
            name = data[at:end].decode("latin-1") if end >= 0 else ""
        sections.append((virtual, min(size, on_disc), offset, name))
    return base, sections


def at(sections, address):
    """(file offset, bytes to the end of its section) of a virtual address, or None."""
    for start, size, offset, _name in sections:
        if start <= address < start + size:
            return offset + address - start, start + size - address
    return None


def read(data, sections, address, size):
    """The bytes at a virtual address, refusing one outside the executable."""
    found = at(sections, address)
    if found is None or found[1] < size:
        raise ValueError(f"{address:#x} with {size} bytes is not in the executable")
    return data[found[0]:found[0] + size]


def text(data, sections, address, limit=256):
    """The string a virtual address points at."""
    found = at(sections, address)
    if found is None:
        raise ValueError(f"{address:#x} is not in the executable")
    end = data.find(b"\0", found[0], found[0] + min(found[1], limit))
    if end < 0:
        raise ValueError(f"no string at {address:#x}")
    return data[found[0]:end].decode("latin-1")


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    base, sections = load(data)
    count, address = struct.unpack_from("<II", data, 0x160)
    for i in range(count):
        name, _major, _minor, _flags, version = struct.unpack_from(
            "<8sHHHH", data, address - base + 16 * i)
        print(name.rstrip(b"\0").decode("ascii", "replace"), version)
    for virtual, size, _offset, name in sections:
        print("section", name, hex(virtual), size)
    print("sha256", hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
