#!/usr/bin/env python3
"""
assemble.py — build the deployable Mac files for the Emu Control Strip.

Inputs (from the Retro68 build directory and resources/native):
  battery.flt, brightness.flt, brightness_cdev.flt   flat code resources
  EmuSerialGateway.bin                                  MacBinary 'appe'
  resources/native/Battery_Monitor.rsrc               Apple's Battery Monitor module fork
  resources/native/Sound_Volume.rsrc                  Apple's Sound Volume module fork
  resources/native/Brightness.rsrc                    Apple's Brightness control panel fork

Outputs (dist/<layout>):
  extfs/   files + .rsrc/ + .finf/ sidecars for SheepShaver's Unix volume
  macbinary/  the same files as MacBinary II (.bin)

Usage:
  assemble.py --build BUILD_DIR --native resources/native --out dist --version 1.0.0
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macres  # noqa: E402
from macres import Resource, ResourceFork, fourcc, pstr  # noqa: E402

COPYRIGHT = "\u00a9 2026 Scott Chamberlain"

BATTERY_NAME = "Emu Battery"
BRIGHTNESS_NAME = "Emu Brightness"
CDEV_NAME = "Brightness"
GATEWAY_NAME = "Emu Serial Gateway"
POWERMGR_NAME = "Emu Power Manager"

BATTERY_STRINGS = [
    "Battery Monitor\r\rYour computer is running off its battery.  The battery is not being charged because the power adapter is not connected.",
    "Battery Monitor\r\rThe power adapter is connected and recharging the battery.",
    "Battery Monitor\r\rThe power adapter is connected to your computer.  The battery is fully charged.",
    "Battery Monitor\r\rThe Emu Serial Gateway is not running, so the battery level is not available.",
    "Battery Monitor\r\rThe battery level is not available at the moment.",
    "Show Battery Level",
    "Hide Battery Level",
    "Show Battery Consumption",
    "Hide Battery Consumption",
    "Show Time Remaining",
    "Hide Time Remaining",
    "Emu Battery Preferences",
]

BRIGHTNESS_STRINGS = [
    "Brightness\r\rTo change the screen brightness, click here and choose another setting.",
    "Brightness\r\rThe Emu Serial Gateway is not running, so the screen brightness cannot be changed from here.",
]


def die(msg: str) -> None:
    sys.stderr.write(f"assemble.py: error: {msg}\n")
    sys.exit(1)


def read(path: str) -> bytes:
    if not os.path.isfile(path):
        die(f"missing input {path}")
    return open(path, "rb").read()


def check_flat_code(blob: bytes, what: str) -> None:
    """Sanity checks on a Retro68 --mac-flat output."""
    if len(blob) < 64:
        die(f"{what}: flat code is only {len(blob)} bytes")
    # Retro68's flat linker script starts with: NOP or _Debugger (0x4E71 / 0xA9FF),
    # then BSR *+2 (0x61000002), ADDI.L (0x0697), displacement.
    first = int.from_bytes(blob[0:2], "big")
    if first not in (0x4E71, 0xA9FF):
        die(f"{what}: unexpected first word 0x{first:04X} (not a Retro68 flat entry trampoline)")
    if blob[2:6] != b"\x61\x00\x00\x02" or blob[6:8] != b"\x06\x97":
        die(f"{what}: entry trampoline not recognised; was it linked with -Wl,--mac-flat?")
    if len(blob) > 32 * 1024 * 8:
        die(f"{what}: implausibly large ({len(blob)} bytes)")


def version_tuple(v: str):
    parts = v.split(".")
    while len(parts) < 3:
        parts.append("0")
    try:
        return tuple(int(p) for p in parts[:3])
    except ValueError:
        die(f"bad version {v!r}")


def add_common(fork: ResourceFork, creator: str, version: str, name: str) -> None:
    major, minor, bug = version_tuple(version)
    long_ = f"{version}, {COPYRIGHT}"
    fork.add(Resource(fourcc("vers"), 1, macres.encode_vers(major, minor, bug, version, long_)), replace=True)
    fork.add(Resource(fourcc("vers"), 2, macres.encode_vers(major, minor, bug, version, "Emu Control Strip")), replace=True)
    # Owner resource: shown by the Finder's Get Info ("Version")
    fork.add(Resource(fourcc(creator), 0, pstr(long_)), replace=True)


# ---- Sun tile icon derived from Apple's Brightness control panel icon -------

def derive_sun_icons(cdev: ResourceFork):
    """Return (ics#, ics4, ics8) bytes for a 16x16 sun tile icon.

    Apple's 32x32 Brightness icon contains a 17x17 sun (rows 7..23, cols
    12..28).  A 16x16 tile cannot hold a symmetric 17x17 glyph, so the outer
    ray tip on each axis is dropped, giving a symmetric 15x15 sun placed at
    the top-left of the 16x16 cell.  The mask is the rays plus the filled
    disc, so the strip background shows around the sun.
    """
    icn = cdev.get("ICN#", -4064)
    icl4 = cdev.get("icl4", -4064)
    icl8 = cdev.get("icl8", -4064)
    if icn is None or icl4 is None or icl8 is None:
        die("Brightness.rsrc lacks the -4064 icon family")
    img1 = macres.unpack_1bit(icn.data, 32, 0)
    img4 = macres.unpack_4bit(icl4.data, 32)
    img8 = macres.unpack_8bit(icl8.data, 32)

    # Source window: rows 8..22, cols 13..27 (15x15)
    r0, c0, n = 8, 13, 15
    sun1 = macres.blank(16)
    sun4 = macres.blank(16)
    sun8 = macres.blank(16)
    for y in range(n):
        for x in range(n):
            sun1[y][x] = img1[r0 + y][c0 + x]
            sun4[y][x] = img4[r0 + y][c0 + x]
            sun8[y][x] = img8[r0 + y][c0 + x]

    # Sanity: the disc outline must be closed, otherwise the fill leaks.
    centre = (15 - r0, 20 - c0)   # original sun centre (row 15, col 20)
    inside = macres.flood_fill_inside(sun1, centre)
    filled = sum(v for row in inside for v in row)
    if filled == 0 or filled > 100:
        die(f"sun disc flood fill produced {filled} pixels; icon layout differs from expected")

    mask = [[1 if (sun1[y][x] or inside[y][x]) else 0 for x in range(16)] for y in range(16)]

    # Colour pixels outside the mask are irrelevant; zero them for tidiness.
    for y in range(16):
        for x in range(16):
            if not mask[y][x]:
                sun4[y][x] = 0
                sun8[y][x] = 0

    ics_hash = macres.pack_1bit(sun1) + macres.pack_1bit(mask)
    return ics_hash, macres.pack_4bit(sun4), macres.pack_8bit(sun8)


def compose_module_finder_icon(sound: ResourceFork, cdev: ResourceFork):
    """Finder icon for the brightness module: Apple's Control Strip module
    plaque (from Sound Volume) with the speaker replaced by the sun."""
    out = {}
    plaque1 = sound.get("ICN#", 128)
    plaque4 = sound.get("icl4", 128)
    plaque8 = sound.get("icl8", 128)
    sun1 = cdev.get("ICN#", -4064)
    sun4 = cdev.get("icl4", -4064)
    sun8 = cdev.get("icl8", -4064)
    if not all([plaque1, plaque4, plaque8, sun1, sun4, sun8]):
        die("icon resources for the module Finder icon are missing")

    p1 = macres.unpack_1bit(plaque1.data, 32, 0)
    pm = macres.unpack_1bit(plaque1.data, 32, 1)
    p4 = macres.unpack_4bit(plaque4.data, 32)
    p8 = macres.unpack_8bit(plaque8.data, 32)
    s1 = macres.unpack_1bit(sun1.data, 32, 0)
    s4 = macres.unpack_4bit(sun4.data, 32)
    s8 = macres.unpack_8bit(sun8.data, 32)

    # Interior background colour: sample a pixel inside the plaque away from the speaker.
    bg4, bg8 = p4[10][10], p8[10][10]
    # Clear the speaker region (rows 6..23, cols 8..28)
    for y in range(6, 24):
        for x in range(8, 29):
            p1[y][x] = 0
            p4[y][x] = bg4
            p8[y][x] = bg8
    # Interior of the sun disc in the source (bounded by the outline ring)
    inside = macres.flood_fill_inside(s1, (15, 20))
    filled = sum(v for row in inside for v in row)
    if filled == 0 or filled > 100:
        die(f"sun disc flood fill produced {filled} pixels; icon layout differs from expected")
    # Paste the 17x17 sun (source rows 7..23, cols 12..28) at rows 7..23, cols 10..26:
    # outline/ray pixels and the coloured disc interior only, never the source background.
    for y in range(17):
        for x in range(17):
            sy, sx = 7 + y, 12 + x
            dy, dx = 7 + y, 10 + x
            if s1[sy][sx]:
                p1[dy][dx] = 1
                p4[dy][dx] = s4[sy][sx]
                p8[dy][dx] = s8[sy][sx]
            elif inside[sy][sx]:
                p4[dy][dx] = s4[sy][sx]
                p8[dy][dx] = s8[sy][sx]
    out["ICN#"] = macres.pack_1bit(p1) + macres.pack_1bit(pm)
    out["icl4"] = macres.pack_4bit(p4)
    out["icl8"] = macres.pack_8bit(p8)
    return out


# ---- Builders -----------------------------------------------------------------

def build_battery(native: str, build: str, version: str) -> ResourceFork:
    apple = ResourceFork.parse(read(os.path.join(native, "Battery_Monitor.rsrc")))
    code = read(os.path.join(build, "battery.flt"))
    check_flat_code(code, "battery.flt")

    f = ResourceFork()
    for t in ("ICN#", "icl4", "icl8"):
        for rid in (128, 270, 271, 272, 273, 274, 275, 276):
            if f.copy_from(apple, t, rid) != 1:
                die(f"Battery_Monitor.rsrc lacks {t} {rid}")
    for t in ("ics#", "ics4", "ics8"):
        for rid in (128, 256, 257, 258, 259, 260):
            if f.copy_from(apple, t, rid) != 1:
                die(f"Battery_Monitor.rsrc lacks {t} {rid}")
    if f.copy_from(apple, "PICT", 256) != 1:
        die("Battery_Monitor.rsrc lacks PICT 256")
    if f.copy_from(apple, "MENU", 256) != 1:
        die("Battery_Monitor.rsrc lacks MENU 256")
    if f.copy_from(apple, "FREF", 128) != 1:
        die("Battery_Monitor.rsrc lacks FREF 128")

    f.add(Resource(fourcc("sdev"), 0, code, None, macres.resSysHeap | macres.resLocked))
    f.add(Resource(fourcc("STR#"), 256, macres.encode_strlist(BATTERY_STRINGS)))
    f.add(Resource(fourcc("BNDL"), 128, macres.encode_bndl("EmBt", [(0, 128)], [(0, 128)])))
    add_common(f, "EmBt", version, BATTERY_NAME)
    return f


def build_brightness(native: str, build: str, version: str) -> ResourceFork:
    sound = ResourceFork.parse(read(os.path.join(native, "Sound_Volume.rsrc")))
    cdev = ResourceFork.parse(read(os.path.join(native, "Brightness.rsrc")))
    code = read(os.path.join(build, "brightness.flt"))
    check_flat_code(code, "brightness.flt")

    f = ResourceFork()
    if f.copy_from(sound, "PICT", 256) != 1:
        die("Sound_Volume.rsrc lacks PICT 256")
    if f.copy_from(sound, "MENU", 256) != 1:
        die("Sound_Volume.rsrc lacks MENU 256")
    if f.copy_from(sound, "FREF", 128) != 1:
        die("Sound_Volume.rsrc lacks FREF 128")

    finder = compose_module_finder_icon(sound, cdev)
    for t, data in finder.items():
        f.add(Resource(fourcc(t), 128, data))
    for t in ("ics#", "ics4", "ics8"):
        if f.copy_from(cdev, t, -4064, new_id=128) != 1:
            die(f"Brightness.rsrc lacks {t} -4064")

    ics_hash, ics4, ics8 = derive_sun_icons(cdev)
    f.add(Resource(fourcc("ics#"), 256, ics_hash))
    f.add(Resource(fourcc("ics4"), 256, ics4))
    f.add(Resource(fourcc("ics8"), 256, ics8))

    f.add(Resource(fourcc("sdev"), 0, code, None, macres.resSysHeap | macres.resLocked))
    f.add(Resource(fourcc("STR#"), 256, macres.encode_strlist(BRIGHTNESS_STRINGS)))
    f.add(Resource(fourcc("BNDL"), 128, macres.encode_bndl("EmBr", [(0, 128)], [(0, 128)])))
    add_common(f, "EmBr", version, BRIGHTNESS_NAME)
    return f


def build_cdev(native: str, build: str, version: str) -> ResourceFork:
    apple = ResourceFork.parse(read(os.path.join(native, "Brightness.rsrc")))
    code = read(os.path.join(build, "brightness_cdev.flt"))
    check_flat_code(code, "brightness_cdev.flt")

    f = ResourceFork()
    for key, r in apple.resources.items():
        t = r.rtype.decode("mac_roman")
        if t in ("cdev", "brit", "BNDL", "vers"):
            continue
        f.add(Resource(r.rtype, r.rid, r.data, r.name, r.attrs))

    required = [("DITL", -4064), ("DITL", -4047), ("DLOG", -4047), ("CNTL", -4048), ("CNTL", -4047),
                ("CDEF", 3), ("sldr", -4048), ("PICT", -4048), ("PICT", -4044), ("PICT", -4043),
                ("nrct", -4064), ("mach", -4064), ("ICN#", -4064), ("FREF", -4064), ("STR#", -4064)]
    for t, rid in required:
        if f.get(t, rid) is None:
            die(f"Brightness.rsrc lacks {t} {rid}")
    mach = f.get("mach", -4064)
    if mach.data != b"\x00\x00\xff\xff":
        die(f"unexpected mach resource {mach.data.hex()}; expected 0000FFFF (cdev decides via macDev)")

    f.add(Resource(fourcc("cdev"), -4064, code, None, macres.resLocked))
    f.add(Resource(fourcc("BNDL"), -4064, macres.encode_bndl("EmBp", [(0, -4064)], [(0, -4064)])))
    add_common(f, "EmBp", version, CDEV_NAME)
    return f


# Original 32x32 icon for the extension: a battery with a lightning bolt.
# '#' outline/black, 'y' yellow fill, 'o' orange bolt, '.' transparent.
POWERMGR_ICON = [
    "................................",
    "................................",
    "............########............",
    "............#yyyyyy#............",
    "..........############..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyoooyy#..........",
    "..........#yyyyooooyy#..........",
    "..........#yyyooooyyy#..........",
    "..........#yyooooyyyy#..........",
    "..........#yyooooooo##..........",
    "..........#yyyyyooooy#..........",
    "..........#yyyyoooyyy#..........",
    "..........#yyyooooyyy#..........",
    "..........#yyyoooyyyy#..........",
    "..........#yyyooyyyyy#..........",
    "..........#yyyoyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........#yyyyyyyyyy#..........",
    "..........############..........",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
]


def powermgr_icons():
    """ICN# (1-bit + mask), icl4, icl8 from the ASCII art above."""
    yellow8, orange8, black8, white8 = 5, 17, 255, 0      # system CLUT: FFFF00, FF9900
    yellow4, orange4, black4, white4 = 1, 2, 15, 0
    img1 = macres.blank(32); mask = macres.blank(32); im4 = macres.blank(32); im8 = macres.blank(32)
    for y, row in enumerate(POWERMGR_ICON):
        for x, c in enumerate(row):
            if c == ".":
                continue
            mask[y][x] = 1
            if c == "#":
                img1[y][x] = 1; im4[y][x] = black4; im8[y][x] = black8
            elif c == "o":
                img1[y][x] = 1; im4[y][x] = orange4; im8[y][x] = orange8
            else:
                im4[y][x] = yellow4; im8[y][x] = yellow8
    return macres.pack_1bit(img1) + macres.pack_1bit(mask), macres.pack_4bit(im4), macres.pack_8bit(im8)


def build_powermgr(build: str, version: str) -> ResourceFork:
    code = read(os.path.join(build, "powermgr.flt"))
    check_flat_code(code, "powermgr.flt")
    icn, icl4, icl8 = powermgr_icons()
    f = ResourceFork()
    f.add(Resource(fourcc("INIT"), 128, code, None, macres.resSysHeap | macres.resLocked))
    f.add(Resource(fourcc("ICN#"), 128, icn))
    f.add(Resource(fourcc("icl4"), 128, icl4))
    f.add(Resource(fourcc("icl8"), 128, icl8))
    f.add(Resource(fourcc("FREF"), 128, macres.encode_fref("INIT", 0)))
    f.add(Resource(fourcc("BNDL"), 128, macres.encode_bndl("EmPm", [(0, 128)], [(0, 128)])))
    add_common(f, "EmPm", version, POWERMGR_NAME)
    return f


def build_gateway(build: str, version: str):
    mb = macres.parse_macbinary(read(os.path.join(build, "EmuSerialGateway.bin")))
    if mb.ftype != b"APPL":
        die(f"EmuSerialGateway.bin has file type {mb.ftype!r}, expected 'APPL' (check add_application TYPE)")
    if mb.creator != b"EmGw":
        die(f"EmuSerialGateway.bin has creator {mb.creator!r}, expected 'EmGw'")
    f = ResourceFork.parse(mb.rsrc)
    if not f.of_type("CODE"):
        die("EmuSerialGateway.bin has no CODE resources")
    size = f.get("SIZE", -1)
    if size is None or len(size.data) < 10:
        die("EmuSerialGateway.bin lacks a SIZE -1 resource")
    flags = int.from_bytes(size.data[0:2], "big")
    if flags & 0x0400:
        die(f"SIZE flags 0x{flags:04X}: the gateway must not be onlyBackground (see gateway.c header)")
    if not (flags & 0x1000):
        die(f"SIZE flags 0x{flags:04X}: canBackground bit not set")
    if not (flags & 0x0040):
        die(f"SIZE flags 0x{flags:04X}: isHighLevelEventAware bit not set")
    add_common(f, "EmGw", version, GATEWAY_NAME)
    return f, mb.data


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, help="Retro68 CMake build directory")
    ap.add_argument("--native", required=True, help="directory with Apple's .rsrc forks")
    ap.add_argument("--out", required=True, help="output directory (dist)")
    ap.add_argument("--version", required=True)
    args = ap.parse_args()

    extfs = os.path.join(args.out, "extfs")
    macbin = os.path.join(args.out, "macbinary")
    os.makedirs(extfs, exist_ok=True)
    os.makedirs(macbin, exist_ok=True)

    items = []
    items.append((BATTERY_NAME, "sdev", "EmBt", build_battery(args.native, args.build, args.version).build(), b""))
    items.append((BRIGHTNESS_NAME, "sdev", "EmBr", build_brightness(args.native, args.build, args.version).build(), b""))
    items.append((CDEV_NAME, "cdev", "EmBp", build_cdev(args.native, args.build, args.version).build(), b""))
    gw_fork, gw_data = build_gateway(args.build, args.version)
    items.append((GATEWAY_NAME, "APPL", "EmGw", gw_fork.build(), gw_data))
    items.append((POWERMGR_NAME, "INIT", "EmPm", build_powermgr(args.build, args.version).build(), b""))

    for name, ftype, creator, rsrc, data in items:
        # verify what we wrote parses back
        fork = ResourceFork.parse(rsrc)
        flags = macres.kHasBundle if fork.of_type("BNDL") else 0
        macres.write_extfs_file(extfs, name, ftype, creator, rsrc, data, finder_flags=flags)
        macres.write_macbinary(os.path.join(macbin, name.replace(" ", "_") + ".bin"),
                              name, ftype, creator, rsrc, data, finder_flags=flags)
        print(f"  {name:20s} {ftype}/{creator}  rsrc {len(rsrc):6d} bytes")
    print(f"assembled into {args.out}")


if __name__ == "__main__":
    main()
