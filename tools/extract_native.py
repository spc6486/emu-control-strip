#!/usr/bin/env python3
"""
extract_native.py — pull the Apple resource forks the build needs out of a
System 7.5.5 disk image you own.  They are not distributed with this
repository.

    extract_native.py /path/to/MacOS755.hda [--out resources/native]

Accepts a raw HFS volume image or an Apple-partitioned image (SheepShaver
.hda/.dsk with an Apple Partition Map); the first Apple_HFS partition is
used.  Extracted:

    System Folder:Control Strip Modules:Battery Monitor  -> Battery_Monitor.rsrc
    System Folder:Control Strip Modules:Sound Volume     -> Sound_Volume.rsrc
    System Folder:Control Panels:Brightness              -> Brightness.rsrc
       (also looked for in Control Panels (Disabled))

Each fork is checked for the resources assemble.py relies on before it is
written.  Requires machfs (pip install machfs).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macres  # noqa: E402

REQUIRED = {
    "Battery_Monitor.rsrc": [("sdev", 0), ("PICT", 256), ("MENU", 256), ("FREF", 128)]
                            + [(t, i) for t in ("ICN#", "icl4", "icl8") for i in (128, 270, 271, 272, 273, 274, 275, 276)]
                            + [(t, i) for t in ("ics#", "ics4", "ics8") for i in (128, 256, 257, 258, 259, 260)],
    "Sound_Volume.rsrc": [("sdev", 0), ("PICT", 256), ("MENU", 256), ("FREF", 128)]
                         + [(t, 128) for t in ("ICN#", "icl4", "icl8")],
    "Brightness.rsrc": [("cdev", -4064), ("DITL", -4064), ("DITL", -4047), ("DLOG", -4047), ("CNTL", -4048),
                        ("CNTL", -4047), ("CDEF", 3), ("sldr", -4048), ("PICT", -4048), ("PICT", -4044),
                        ("PICT", -4043), ("nrct", -4064), ("mach", -4064), ("FREF", -4064), ("STR#", -4064)]
                       + [(t, -4064) for t in ("ICN#", "icl4", "icl8", "ics#", "ics4", "ics8")],
}


def die(msg: str) -> None:
    sys.stderr.write(f"extract_native.py: error: {msg}\n")
    sys.exit(1)


def hfs_volume_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        head = f.read(4096)
        if head[:2] == b"ER":                       # Apple driver descriptor -> partition map
            blksize = struct.unpack(">H", head[2:4])[0]
            f.seek(blksize)
            first = f.read(blksize)
            if first[:2] != b"PM":
                die("partitioned image without a partition map")
            npart = struct.unpack(">I", first[4:8])[0]
            for i in range(npart):
                f.seek(blksize * (1 + i))
                e = f.read(blksize)
                start, size = struct.unpack(">II", e[8:16])
                ptype = e[48:80].split(b"\0")[0]
                if ptype == b"Apple_HFS":
                    f.seek(start * blksize)
                    return f.read(size * blksize)
            die("no Apple_HFS partition found")
        if head[1024:1026] == b"BD":                 # raw HFS volume
            f.seek(0)
            return f.read()
    die("not an HFS image (no MDB signature at offset 1024, no partition map)")


def find_file(root, *paths):
    for path in paths:
        node = root
        try:
            for part in path:
                node = node[part]
            return node, ":".join(path)
        except KeyError:
            continue
    return None, None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="System 7.5.5 disk image (raw HFS or Apple-partitioned)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                  "resources", "native"))
    args = ap.parse_args()

    try:
        import machfs
    except ImportError:
        die("machfs not installed (pip install machfs)")

    v = machfs.Volume()
    v.read(hfs_volume_bytes(args.image))
    sf, _ = find_file(v, ("System Folder",))
    if sf is None:
        die("no System Folder on the volume")

    wanted = [
        ("Battery_Monitor.rsrc", [("System Folder", "Control Strip Modules", "Battery Monitor")]),
        ("Sound_Volume.rsrc", [("System Folder", "Control Strip Modules", "Sound Volume")]),
        ("Brightness.rsrc", [("System Folder", "Control Panels", "Brightness"),
                             ("System Folder", "Control Panels (Disabled)", "Brightness")]),
    ]
    os.makedirs(args.out, exist_ok=True)
    for out_name, paths in wanted:
        obj, where = find_file(v, *paths)
        if obj is None:
            die(f"{out_name}: not found at " + " or ".join(":".join(p) for p in paths))
        fork = macres.ResourceFork.parse(obj.rsrc)
        missing = [f"{t} {i}" for t, i in REQUIRED[out_name] if fork.get(t, i) is None]
        if missing:
            die(f"{where}: missing resources {missing} — is this the System 7.5.5 version?")
        with open(os.path.join(args.out, out_name), "wb") as f:
            f.write(obj.rsrc)
        print(f"  {where}  ->  {out_name}  ({len(obj.rsrc)} bytes, {len(fork.resources)} resources)")
    print(f"written to {args.out}")


if __name__ == "__main__":
    main()
