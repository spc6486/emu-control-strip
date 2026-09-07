#!/usr/bin/env python3
"""
macres.py — classic Mac OS resource fork tools used by assemble.py.

  * ResourceFork: parse / build raw resource forks (the format used by HFS
    resource forks, SheepShaver's .rsrc helper files, and Rez output).
  * MacBinary II parsing (Retro68's Rez writes .bin files).
  * Encoders for 'STR#', 'vers', 'BNDL', 'FREF', 'MENU' and owner resources.
  * extfs sidecar output: SheepShaver/Basilisk II on Linux keep a file's
    resource fork in <dir>/.rsrc/<name> and its Finder info (FInfo+FXInfo,
    32 bytes) in <dir>/.finf/<name>.

Only the standard library is used.  Python 3.8+.
"""

from __future__ import annotations

import os
import struct
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

MACROMAN = "mac_roman"

# Resource attribute bits
resSysHeap = 0x40
resPurgeable = 0x20
resLocked = 0x10
resProtected = 0x08
resPreload = 0x04
resChanged = 0x02

# Finder flags
kHasBundle = 0x2000
kIsInvisible = 0x4000


def fourcc(s: str) -> bytes:
    b = s.encode(MACROMAN)
    if len(b) != 4:
        raise ValueError(f"four-character code must be 4 bytes: {s!r}")
    return b


def pstr(s: str) -> bytes:
    b = s.encode(MACROMAN)
    if len(b) > 255:
        raise ValueError("Pascal string longer than 255 bytes")
    return bytes([len(b)]) + b


@dataclass
class Resource:
    rtype: bytes
    rid: int
    data: bytes
    name: Optional[str] = None
    attrs: int = 0

    def key(self) -> Tuple[bytes, int]:
        return (self.rtype, self.rid)


class ResourceFork:
    """An ordered collection of resources with parse/build."""

    def __init__(self) -> None:
        self.resources: "OrderedDict[Tuple[bytes, int], Resource]" = OrderedDict()

    # ---- access -----------------------------------------------------------
    def add(self, res: Resource, replace: bool = False) -> None:
        k = res.key()
        if k in self.resources and not replace:
            raise KeyError(f"duplicate resource {res.rtype!r} {res.rid}")
        self.resources[k] = res

    def get(self, rtype: str, rid: int) -> Optional[Resource]:
        return self.resources.get((fourcc(rtype), rid))

    def remove(self, rtype: str, rid: Optional[int] = None) -> int:
        t = fourcc(rtype)
        keys = [k for k in self.resources if k[0] == t and (rid is None or k[1] == rid)]
        for k in keys:
            del self.resources[k]
        return len(keys)

    def types(self) -> List[bytes]:
        seen: List[bytes] = []
        for (t, _rid) in self.resources:
            if t not in seen:
                seen.append(t)
        return seen

    def of_type(self, rtype: str) -> List[Resource]:
        t = fourcc(rtype)
        return [r for (rt, _), r in self.resources.items() if rt == t]

    def copy_from(self, other: "ResourceFork", rtype: str, rid: Optional[int] = None,
                  new_id: Optional[int] = None, attrs: Optional[int] = None) -> int:
        """Copy resources of a type (optionally one ID, optionally renumbered)."""
        n = 0
        for r in other.of_type(rtype):
            if rid is not None and r.rid != rid:
                continue
            nr = Resource(r.rtype, new_id if new_id is not None else r.rid, r.data, r.name,
                          r.attrs if attrs is None else attrs)
            self.add(nr, replace=True)
            n += 1
        return n

    # ---- parse ------------------------------------------------------------
    @classmethod
    def parse(cls, data: bytes) -> "ResourceFork":
        fork = cls()
        if len(data) < 16:
            if len(data) == 0:
                return fork
            raise ValueError("resource fork too short")
        data_off, map_off, data_len, map_len = struct.unpack(">IIII", data[:16])
        if map_off + map_len > len(data) or data_off + data_len > len(data):
            raise ValueError("resource fork header out of range")
        m = data[map_off:map_off + map_len]
        if len(m) < 30:
            raise ValueError("resource map too short")
        type_list_off, name_list_off = struct.unpack(">HH", m[24:28])
        ntypes = struct.unpack(">H", m[type_list_off:type_list_off + 2])[0] + 1
        if ntypes == 0x10000:  # count word was 0xFFFF -> no types
            return fork
        p = type_list_off + 2
        for _ in range(ntypes):
            rtype, count, ref_off = struct.unpack(">4sHH", m[p:p + 8])
            p += 8
            count += 1
            rp = type_list_off + ref_off
            for _ in range(count):
                rid, name_off, attr_off = struct.unpack(">hHI", m[rp:rp + 8])
                rp += 12
                attrs = attr_off >> 24
                off = attr_off & 0xFFFFFF
                name = None
                if name_off != 0xFFFF:
                    nl = m[name_list_off + name_off]
                    name = m[name_list_off + name_off + 1:name_list_off + name_off + 1 + nl].decode(MACROMAN)
                start = data_off + off
                rlen = struct.unpack(">I", data[start:start + 4])[0]
                rdata = data[start + 4:start + 4 + rlen]
                if len(rdata) != rlen:
                    raise ValueError(f"resource {rtype!r} {rid} truncated")
                fork.add(Resource(rtype, rid, rdata, name, attrs), replace=True)
        return fork

    # ---- build ------------------------------------------------------------
    def build(self) -> bytes:
        # Data section
        data_parts: List[bytes] = []
        offsets: Dict[Tuple[bytes, int], int] = {}
        pos = 0
        for k, r in self.resources.items():
            offsets[k] = pos
            chunk = struct.pack(">I", len(r.data)) + r.data
            data_parts.append(chunk)
            pos += len(chunk)
        data_section = b"".join(data_parts)

        # Names
        names = bytearray()
        name_offsets: Dict[Tuple[bytes, int], int] = {}
        for k, r in self.resources.items():
            if r.name is not None:
                name_offsets[k] = len(names)
                names += pstr(r.name)

        types = self.types()
        type_list_len = 2 + 8 * len(types)
        ref_lists = bytearray()
        type_entries = bytearray()
        for t in types:
            items = [r for (rt, _), r in self.resources.items() if rt == t]
            ref_off = type_list_len + len(ref_lists)
            type_entries += struct.pack(">4sHH", t, len(items) - 1, ref_off)
            for r in items:
                k = r.key()
                noff = name_offsets.get(k, 0xFFFF)
                ref_lists += struct.pack(">hHI", r.rid, noff,
                                         ((r.attrs & 0xFF) << 24) | (offsets[k] & 0xFFFFFF)) + b"\0\0\0\0"
        type_list = struct.pack(">H", len(types) - 1 if types else 0xFFFF) + bytes(type_entries)

        map_header_len = 28
        name_list_off = map_header_len + len(type_list) + len(ref_lists)
        map_body = bytes(type_list) + bytes(ref_lists) + bytes(names)

        data_off = 256
        map_off = data_off + len(data_section)
        map_len = map_header_len + len(map_body)
        header = struct.pack(">IIII", data_off, map_off, len(data_section), map_len)
        header += b"\0" * (256 - 16)
        map_header = header[:16] + b"\0\0\0\0" + b"\0\0" + b"\0\0" + struct.pack(">HH", map_header_len, name_list_off)
        return header + data_section + map_header + map_body


# ---- MacBinary ------------------------------------------------------------

@dataclass
class MacBinaryFile:
    name: str
    ftype: bytes
    creator: bytes
    flags: int
    data: bytes
    rsrc: bytes


def parse_macbinary(blob: bytes) -> MacBinaryFile:
    if len(blob) < 128:
        raise ValueError("not a MacBinary file (too short)")
    h = blob[:128]
    if h[0] != 0:
        raise ValueError("not a MacBinary file (bad version byte)")
    nlen = h[1]
    if nlen == 0 or nlen > 63:
        raise ValueError("not a MacBinary file (bad name length)")
    name = h[2:2 + nlen].decode(MACROMAN)
    ftype = h[65:69]
    creator = h[69:73]
    flags = (h[73] << 8) | h[101]
    dlen, rlen = struct.unpack(">II", h[83:91])
    dstart = 128
    dend = dstart + dlen
    rstart = dend + ((128 - (dlen % 128)) % 128)
    rend = rstart + rlen
    if rend > len(blob):
        raise ValueError("MacBinary forks exceed file size")
    return MacBinaryFile(name, ftype, creator, flags, blob[dstart:dend], blob[rstart:rend])


# ---- Encoders ---------------------------------------------------------------

def encode_strlist(strings: Iterable[str]) -> bytes:
    items = list(strings)
    return struct.pack(">H", len(items)) + b"".join(pstr(s) for s in items)


def encode_vers(major: int, minor: int, bugfix: int, short: str, long_: str,
                stage: int = 0x80, nonrel: int = 0, region: int = 0) -> bytes:
    bcd_major = ((major // 10) << 4) | (major % 10)
    return (bytes([bcd_major, ((minor & 0xF) << 4) | (bugfix & 0xF), stage, nonrel])
            + struct.pack(">H", region) + pstr(short) + pstr(long_))


def encode_bndl(creator: str, icon_ids: List[Tuple[int, int]], fref_ids: List[Tuple[int, int]]) -> bytes:
    out = fourcc(creator) + struct.pack(">hH", 0, 1)   # owner ID 0, two types
    out += fourcc("ICN#") + struct.pack(">H", len(icon_ids) - 1)
    for local, rid in icon_ids:
        out += struct.pack(">hh", local, rid)
    out += fourcc("FREF") + struct.pack(">H", len(fref_ids) - 1)
    for local, rid in fref_ids:
        out += struct.pack(">hh", local, rid)
    return out


def encode_fref(ftype: str, local_id: int = 0, filename: str = "") -> bytes:
    return fourcc(ftype) + struct.pack(">h", local_id) + pstr(filename)


def encode_menu(menu_id: int, items: List[str], proc_id: int = 0, title: str = "") -> bytes:
    out = struct.pack(">hhhhhI", menu_id, 0, 0, proc_id, 0, 0xFFFFFFFF) + pstr(title)
    for it in items:
        out += pstr(it) + bytes([0, 0, 0, 0])
    out += b"\0"
    return out


def decode_strlist(data: bytes) -> List[str]:
    n = struct.unpack(">H", data[:2])[0]
    out = []
    i = 2
    for _ in range(n):
        ln = data[i]
        out.append(data[i + 1:i + 1 + ln].decode(MACROMAN))
        i += 1 + ln
    return out


# ---- Icons ------------------------------------------------------------------

def unpack_1bit(data: bytes, size: int, plane: int = 0) -> List[List[int]]:
    """1-bit icon plane (plane 0 = image, 1 = mask) -> rows of 0/1."""
    rowbytes = size // 8
    base = plane * size * rowbytes
    return [[(data[base + y * rowbytes + x // 8] >> (7 - x % 8)) & 1 for x in range(size)] for y in range(size)]


def pack_1bit(rows: List[List[int]]) -> bytes:
    size = len(rows)
    rowbytes = size // 8
    out = bytearray(size * rowbytes)
    for y in range(size):
        for x in range(size):
            if rows[y][x]:
                out[y * rowbytes + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def unpack_8bit(data: bytes, size: int) -> List[List[int]]:
    return [[data[y * size + x] for x in range(size)] for y in range(size)]


def pack_8bit(rows: List[List[int]]) -> bytes:
    return bytes(v for row in rows for v in row)


def unpack_4bit(data: bytes, size: int) -> List[List[int]]:
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            b = data[(y * size + x) // 2]
            row.append(b >> 4 if x % 2 == 0 else b & 0xF)
        rows.append(row)
    return rows


def pack_4bit(rows: List[List[int]]) -> bytes:
    size = len(rows)
    out = bytearray(size * size // 2)
    for y in range(size):
        for x in range(size):
            idx = (y * size + x) // 2
            if x % 2 == 0:
                out[idx] |= (rows[y][x] & 0xF) << 4
            else:
                out[idx] |= rows[y][x] & 0xF
    return bytes(out)


def blank(size: int, value: int = 0) -> List[List[int]]:
    return [[value] * size for _ in range(size)]


def flood_fill_inside(icon: List[List[int]], start: Tuple[int, int]) -> List[List[int]]:
    """4-connected flood fill of zero pixels starting at `start`, bounded by
    set pixels and the image edge.  Returns the filled region as 0/1 rows."""
    size = len(icon)
    region = blank(size)
    stack = [start]
    while stack:
        y, x = stack.pop()
        if y < 0 or y >= size or x < 0 or x >= size:
            continue
        if icon[y][x] or region[y][x]:
            continue
        region[y][x] = 1
        stack.extend([(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)])
    return region


# ---- extfs sidecars ---------------------------------------------------------

def write_extfs_file(directory: str, name: str, ftype: str, creator: str,
                     rsrc: bytes, data: bytes = b"", finder_flags: int = 0) -> None:
    """Write name, .rsrc/name and .finf/name so SheepShaver's Unix volume
    presents a proper Mac file with type/creator and resource fork."""
    os.makedirs(directory, exist_ok=True)
    os.makedirs(os.path.join(directory, ".rsrc"), exist_ok=True)
    os.makedirs(os.path.join(directory, ".finf"), exist_ok=True)
    with open(os.path.join(directory, name), "wb") as f:
        f.write(data)
    with open(os.path.join(directory, ".rsrc", name), "wb") as f:
        f.write(rsrc)
    finfo = fourcc(ftype) + fourcc(creator) + struct.pack(">Hhhh", finder_flags, -1, -1, 0)
    fxinfo = b"\0" * 16
    with open(os.path.join(directory, ".finf", name), "wb") as f:
        f.write(finfo + fxinfo)


def write_macbinary(path: str, name: str, ftype: str, creator: str, rsrc: bytes,
                    data: bytes = b"", finder_flags: int = 0) -> None:
    """Write a MacBinary II file (for transfer through tools that understand it)."""
    nameb = name.encode(MACROMAN)[:63]
    h = bytearray(128)
    h[1] = len(nameb)
    h[2:2 + len(nameb)] = nameb
    h[65:69] = fourcc(ftype)
    h[69:73] = fourcc(creator)
    h[73] = (finder_flags >> 8) & 0xFF
    h[101] = finder_flags & 0xFF
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    # Creation/modification dates: seconds since 1904-01-01 (Mac epoch)
    import time
    mac_now = int(time.time()) + 2082844800
    struct.pack_into(">II", h, 91, mac_now, mac_now)
    h[122] = 129
    h[123] = 129
    crc = _crc16_xmodem(bytes(h[:124]))
    struct.pack_into(">H", h, 124, crc)

    def pad(b: bytes) -> bytes:
        return b + b"\0" * ((128 - len(b) % 128) % 128)

    with open(path, "wb") as f:
        f.write(bytes(h) + pad(data) + pad(rsrc))


def _crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


# ---- self test --------------------------------------------------------------

def _selftest(paths: List[str]) -> None:
    for p in paths:
        raw = open(p, "rb").read()
        fork = ResourceFork.parse(raw)
        rebuilt = fork.build()
        fork2 = ResourceFork.parse(rebuilt)
        assert list(fork.resources.keys()) == list(fork2.resources.keys()), p
        for k in fork.resources:
            a, b = fork.resources[k], fork2.resources[k]
            assert a.data == b.data and a.name == b.name and a.attrs == b.attrs, (p, k)
        print(f"OK {os.path.basename(p)}: {len(fork.resources)} resources round-trip")


if __name__ == "__main__":
    import sys
    _selftest(sys.argv[1:])
