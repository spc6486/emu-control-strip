#!/usr/bin/env python3
"""
make_floppy.py — build the 1.44 MB distribution floppy for the Emu
Control Strip.

Layout (volume "Emu Control Strip"):

    Read Me                          TEXT/ttxt
    Startup Items/Emu Serial Gateway    APPL/EmGw
    Extensions/Emu Power Manager INIT/EmPm
    Control Strip Modules/           Emu Battery, Emu Brightness (sdev)
    Control Panels/Brightness        cdev/EmBp
    Developer Notes/                 Apple Events, Status Block, Power Manager, EmuShared.h (TEXT)

Inputs: dist/macbinary/*.bin from assemble.py, texts from floppy/, the shared
header.  Requires hfsutils (hformat, hmount, hmkdir, hcopy, humount) — part
of the Retro68 toolchain — and machfs for verification.

Outputs: <out>/<name>.img (raw HFS, 1,474,560 bytes) and <out>/<name>.image
(DiskCopy 4.2, same data with header and checksums; untested on real
hardware).
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macres  # noqa: E402

FLOPPY_BYTES = 1474560
VOLUME_NAME = "Emu Control Strip"

MAC_FILES = [
    # (macbinary file, destination folder on the floppy)
    ("Emu_Serial_Gateway.bin", "Startup Items"),
    ("Emu_Power_Manager.bin", "Extensions"),
    ("Emu_Battery.bin", "Control Strip Modules"),
    ("Emu_Brightness.bin", "Control Strip Modules"),
    ("Brightness.bin", "Control Panels"),
]


def die(msg: str) -> None:
    sys.stderr.write(f"make_floppy.py: error: {msg}\n")
    sys.exit(1)


def run(cmd: list[str], env: dict) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        die(f"{' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def mac_text(text: str, version: str, date: str) -> bytes:
    """Substitute placeholders, use CR line endings, encode Mac Roman."""
    text = text.replace("{VERSION}", version).replace("{DATE}", date)
    text = text.replace("\r\n", "\n").replace("\n", "\r")
    return text.encode("mac_roman")


def text_macbinary(tmpdir: str, name: str, data: bytes) -> str:
    path = os.path.join(tmpdir, name.replace(" ", "_").replace(".", "_") + ".bin")
    macres.write_macbinary(path, name, "TEXT", "ttxt", b"", data)
    return path


def dc42(name: str, data: bytes) -> bytes:
    """DiskCopy 4.2 container for a 1440K image (no tag bytes)."""
    if len(data) != FLOPPY_BYTES:
        die("DC42: image is not 1440K")

    def checksum(buf: bytes) -> int:
        cs = 0
        for i in range(0, len(buf), 2):
            cs = (cs + struct.unpack(">H", buf[i:i + 2])[0]) & 0xFFFFFFFF
            cs = ((cs >> 1) | ((cs & 1) << 31)) & 0xFFFFFFFF
        return cs

    nameb = name.encode("mac_roman")[:63]
    header = bytes([len(nameb)]) + nameb + b"\0" * (64 - 1 - len(nameb))
    header += struct.pack(">IIII", len(data), 0, checksum(data), 0)
    header += bytes([0x03, 0x22])        # 1440K, MFM double-sided
    header += struct.pack(">H", 0x0100)  # private magic
    assert len(header) == 84
    return header + data


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", required=True, help="dist directory from assemble.py")
    ap.add_argument("--texts", required=True, help="directory with Read Me and Developer Notes sources")
    ap.add_argument("--header", required=True, help="path to EmuShared.h")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--version", required=True)
    ap.add_argument("--date", required=True, help="release date shown in the Read Me")
    ap.add_argument("--hfsutils", default="", help="directory containing hformat/hmount/... (else PATH)")
    args = ap.parse_args()

    env = dict(os.environ)
    if args.hfsutils:
        env["PATH"] = args.hfsutils + os.pathsep + env.get("PATH", "")
    env["HOME"] = tempfile.mkdtemp(prefix="hfsutils-home-")   # private .hcwd
    for tool in ("hformat", "hmount", "hmkdir", "hcopy", "humount"):
        if shutil.which(tool, path=env["PATH"]) is None:
            die(f"{tool} not found (hfsutils); set --hfsutils to the Retro68 toolchain bin directory")

    macbin = os.path.join(args.dist, "macbinary")
    for f, _ in MAC_FILES:
        if not os.path.isfile(os.path.join(macbin, f)):
            die(f"missing {os.path.join(macbin, f)} — run assemble.py first")

    os.makedirs(args.out, exist_ok=True)
    image_name = f"{VOLUME_NAME} {args.version}"
    img_path = os.path.join(args.out, image_name + ".img")
    dc42_path = os.path.join(args.out, image_name + ".image")

    tmpdir = tempfile.mkdtemp(prefix="emu-floppy-")
    texts = {}
    for src_name, dest_name in (("ReadMe.txt", "Read Me"),
                                ("AppleEvents.txt", "Apple Events"),
                                ("StatusBlock.txt", "Status Block"),
                                ("PowerManager.txt", "Power Manager")):
        src = os.path.join(args.texts, src_name)
        if not os.path.isfile(src):
            die(f"missing text source {src}")
        texts[dest_name] = mac_text(open(src, encoding="utf-8").read(), args.version, args.date)
    texts["EmuShared.h"] = mac_text(open(args.header, encoding="utf-8").read(), args.version, args.date)
    for name, data in texts.items():
        if len(data) > 32000:
            die(f"{name} is {len(data)} bytes; SimpleText opens at most 32K")

    # ---- create and populate the image --------------------------------------
    with open(img_path, "wb") as f:
        f.write(b"\0" * FLOPPY_BYTES)
    run(["hformat", "-l", VOLUME_NAME, img_path], env)
    run(["hmount", img_path], env)
    try:
        for folder in ("Startup Items", "Extensions", "Control Strip Modules", "Control Panels", "Developer Notes"):
            run(["hmkdir", f":{folder}"], env)
        for f, folder in MAC_FILES:
            run(["hcopy", "-m", os.path.join(macbin, f), f":{folder}:"], env)
        run(["hcopy", "-m", text_macbinary(tmpdir, "Read Me", texts["Read Me"]), ":"], env)
        for name in ("Apple Events", "Status Block", "Power Manager", "EmuShared.h"):
            run(["hcopy", "-m", text_macbinary(tmpdir, name, texts[name]), ":Developer Notes:"], env)
    finally:
        run(["humount"], env)

    # ---- verify with an independent HFS implementation -----------------------
    try:
        import machfs
    except ImportError:
        die("machfs not installed (pip install machfs)")
    raw = open(img_path, "rb").read()
    if len(raw) != FLOPPY_BYTES:
        die(f"image is {len(raw)} bytes, expected {FLOPPY_BYTES}")
    v = machfs.Volume()
    v.read(raw)
    # Volume name straight from the Master Directory Block (machfs's .name is unreliable)
    if raw[1024:1026] != b"BD":
        die("no HFS signature in the Master Directory Block")
    mdb_name = raw[1024 + 37:1024 + 37 + raw[1024 + 36]].decode("mac_roman")
    if mdb_name != VOLUME_NAME:
        die(f"volume name is {mdb_name!r}")
    forks = os.path.join(args.dist, "extfs", ".rsrc")
    checks = [("Startup Items", "Emu Serial Gateway", b"APPL", b"EmGw"),
              ("Extensions", "Emu Power Manager", b"INIT", b"EmPm"),
              ("Control Strip Modules", "Emu Battery", b"sdev", b"EmBt"),
              ("Control Strip Modules", "Emu Brightness", b"sdev", b"EmBr"),
              ("Control Panels", "Brightness", b"cdev", b"EmBp")]
    for folder, name, ftype, creator in checks:
        obj = v[folder][name]
        if obj.type != ftype or obj.creator != creator:
            die(f"{folder}/{name}: type/creator {obj.type!r}/{obj.creator!r}")
        if obj.rsrc != open(os.path.join(forks, name), "rb").read():
            die(f"{folder}/{name}: resource fork differs from dist")
    for folder, name in ((None, "Read Me"), ("Developer Notes", "Apple Events"),
                         ("Developer Notes", "Status Block"), ("Developer Notes", "Power Manager"),
                         ("Developer Notes", "EmuShared.h")):
        obj = v[name] if folder is None else v[folder][name]
        if obj.type != b"TEXT" or obj.creator != b"ttxt":
            die(f"{name}: not a SimpleText document")
        if obj.data != texts[name]:
            die(f"{name}: text differs")
        if b"\n" in obj.data:
            die(f"{name}: contains LF line endings")
    used = sum(len(o.data) + len(o.rsrc) for o in
               [v["Read Me"]] + [v[f][n] for f, n, _, _ in checks] + list(v["Developer Notes"].values()))

    with open(dc42_path, "wb") as f:
        f.write(dc42(image_name, raw))
    shutil.rmtree(tmpdir, ignore_errors=True)
    print(f"  {img_path}  (raw HFS, {len(raw)} bytes, ~{used // 1024} KB used)")
    print(f"  {dc42_path}  (DiskCopy 4.2, untested on hardware)")


if __name__ == "__main__":
    main()
