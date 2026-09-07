# Emu Control Strip

Native Mac OS 7.5 battery and brightness controls for a Macintosh running
under SheepShaver or Basilisk II, fed by
[emu-serial-bridge](https://github.com/spc6486/emu-serial-bridge) on the host
computer (for example a Raspberry Pi with a battery UPS). The pieces look and
behave like Apple's own because, wherever Apple's code exists, Apple's code
does the drawing:

| Deliverable | Kind | Where it goes | What it is |
|---|---|---|---|
| **Emu Serial Gateway** | application, runs in the background | System Folder:Startup Items | Owns the serial port, polls the host, publishes status, relays commands for other applications |
| **Emu Battery** | `sdev` Control Strip module | System Folder:Control Strip Modules | Apple's Battery Monitor tile, artwork and layout intact, fed by the gateway |
| **Emu Brightness** | `sdev` Control Strip module | System Folder:Control Strip Modules | Sound Volume-style popup (7…0) that sets the backlight |
| **Brightness** | `cdev` control panel | System Folder:Control Panels | Apple's Brightness panel (slider, Shortcut dialog) driving the backlight |
| **Emu Power Manager** | `INIT` system extension | System Folder:Extensions | Presents the PowerBook Power Manager interface so Apple's menu bar clock shows the battery |

## Requirements

- Mac OS 7.5.3 or later with the Control Strip installed (developed and tested on 7.5.5).
- [emu-serial-bridge](https://github.com/spc6486/emu-serial-bridge) on the
  host with the battery protocol `BAT <pct> <CHG|DIS|UNK> <min> <watts> <chg> <shdn>`,
  and the emulator's modem port (`seriala`) mapped to the bridge's PTY.
- [battery-monitor](https://github.com/spc6486/battery-monitor) 2.0 or
  later for the values the bridge reports (time remaining and consumption
  need a UPS backend that provides them, such as the SunFounder PiPower 5).
- To build: Retro68 (68k target), cmake, python3 with `machfs`, and a
  System 7.5.5 disk image of your own (see Building).

## Architecture

```
 Control Strip modules / Brightness cdev
        │  Gestalt('EmuG') → EmuStatus block (system heap)
        │  read status, post requests
        ▼
 Emu Serial Gateway (app)  ──.AIn/.AOut──▶ SheepShaver seriala ──PTY──▶ emu-serial-bridge ──▶ battery-monitor JSON,
        ▲                                                                                  sysfs PWM backlight,
        │  Apple Event relay ('EmuG'/'serl', 'misc'/'eval', 'misc'/'dosc')                  Home Assistant handler
 Other apps  (HyperCard, AppleScript, any AE client) — see docs/apple-events.md
```

- One owner of the serial port. The modules never touch it; other
  applications go through the gateway's Apple Event relay or read its status
  block (`docs/apple-events.md`).
- The status block is validated (signature, version, size) by every reader;
  a missing or stale gateway shows as the "empty battery" icon, a blinking
  level bar, and disabled popup items, exactly as Apple's module behaves when
  the battery is absent.
- **Menu bar battery.** System 7.5's menu bar clock (SuperClock!, folded into
  Date & Time) has a "Show the battery level" option that is dimmed unless
  `Gestalt('powr')` reports a Power Manager at INIT time. The Emu Power
  Manager extension registers `'powr'` (exists + 2.0 dispatch) and installs
  `_PowerMgrDispatch` (selectors 0, 1, 12 `GetScaledBatteryInfo`, 26
  `BatteryCount`), `_Sleep`, `_PMgrOp` and `_PowerDispatch` as system-heap
  stubs that read the status block. Apple's clock then draws its own 17
  battery frames (eight levels, eight charging, one X) from our data:
  frame = level ≫ 5, bolt when installed+charging+charger are all set, X when
  the installed bit is clear. PCI Power Macs already carry a 2.0 dispatch
  (`'powr'` = `$10`, used by Energy Saver), so the extension chains in front
  of it, answering only selectors 12 and 26. Clicking the icon (or Finder ›
  Sleep) calls `_Sleep`: the system's own where it exists, otherwise the
  extension's, which the gateway forwards to the bridge as `SLEEP`.
- Gateway polls: `BAT?` every 10 s, `BRI?` every 3 s, `AUTO?` every 60 s;
  five times slower after three consecutive failures. Requests from the UI
  are sent immediately.
- Serial I/O in the gateway is entirely asynchronous. A read is issued only
  when the driver reports bytes waiting, and a write that has not completed
  after 2 s means the host side is not draining the PTY; the gateway then logs
  `serial: paused` and stops using the port instead of letting a synchronous
  call hang the machine, resuming once the stuck request drains.
- The gateway is a normal application, not a background-only `appe`: under
  SheepShaver the serial driver's open never completed for a faceless process
  launched from the Extensions folder, while identical code launched by the
  Finder worked. It shows an Apple menu and File > Quit, hands the foreground
  back to the Finder on its first event, and appears in the Application menu.

### Faithfulness

The Battery tile reproduces the layout recovered from Apple's Battery Monitor
1.3.1 code: status icon (16 px), etched divider, 8-bar `SBDrawBarGraph`
level with Apple's scaling (`((raw·9)>>8)−1`), 32-px consumption dial, right
justified `H:MM` time remaining, popup arrow; time and consumption are hidden
on external power; the level bar blinks at 1 Hz when unknown. All icons,
PICTs and the MENU come verbatim from Apple's module. The Brightness tile
follows Sound Volume (icon + arrow, bulleted 7…0 popup); its sun is cropped
from Apple's Brightness control panel icon. The control panel keeps Apple's
DITL, slider CDEF, pictures and help strings; only the `cdev` code is new.

## Installation

### From the floppy image (recommended)

Each release ships `Emu Control Strip <version>.img`, a 1.44 MB HFS
floppy (with a DiskCopy 4.2 `.image` alongside) holding the four Mac files in
folders named after their destinations, a Read Me, and Developer Notes. Mount
it in the emulator (a `disk` line in the SheepShaver prefs, or the
emulator-manager), then:

1. `Startup Items:Emu Serial Gateway` → System Folder:Startup Items
2. `Extensions:Emu Power Manager` → System Folder:Extensions
3. `Control Strip Modules:Emu Battery` and `Emu Brightness` →
   System Folder:Control Strip Modules
4. Move Apple's `Brightness` control panel to Control Panels (Disabled) — it
   refuses to open on this machine anyway — then `Control Panels:Brightness`
   → System Folder:Control Panels
5. Because a Power Manager is now advertised, park Apple's PowerBook-only
   pieces: Battery Monitor, Sleep Now, Power Settings and HD Spin Down out of
   Control Strip Modules (e.g. into `Control Strip (Disabled)` — HFS names
   are limited to 31 characters), PowerBook and PowerBook Setup into Control
   Panels (Disabled)
6. Restart. Apple's battery appears next to the clock by itself (the clock's
   battery option defaults to on; Clock Options is where to turn it off).

The bridge must be running before the emulator starts; if the bridge is
restarted later, restart the emulator too (it keeps its old PTY descriptor).

### Over SSH (alternative)

```bash
scp emu-control-strip-<version>.tar.gz <user>@<host>:~
```
```bash
tar -xzf emu-control-strip-<version>.tar.gz && ./install.sh
```

`install.sh` stages the same files in `~/emu-mac` with SheepShaver's
`.rsrc`/`.finf` helper files, visible in the emulator as
`Unix:home:pi:emu-mac`; the drags are the same as above.
`./install.sh --uninstall` removes the staging directory.

## Usage

- **Battery tile:** plain battery on battery power, lightning bolt while
  charging, plug when on external power and not charging; eight-bar level.
  Click for Show/Hide Battery Level, Battery Consumption (dial, ~3–12 W) and
  Time Remaining (`H:MM`). The last two are available on PiPower 5 units and,
  as on a PowerBook, only while on battery.
- **Brightness tile:** popup of levels 7 (brightest) to 0, current level
  bulleted; levels map to 10–100 % on the host.
- **Brightness control panel:** slider between Off and Max, digits 0–9, Up
  and Down Arrow. The Shortcut dialog records modifier keys; the system-wide
  hotkey itself is not yet implemented.
- **Menu bar battery:** Apple's clock battery filled from the charge, bolt
  while charging, X while the gateway has no data; on by default, switchable
  in Date & Time › Clock Options. Clicking it, or Finder › Special › Sleep,
  runs the system's Sleep (Energy Saver's on a PCI Power Mac; elsewhere the
  extension's, forwarded to the bridge as `SLEEP`).
- **Balloon Help** over the battery tile reports whether the gateway is
  running and what the battery is doing.
- A UPS shutdown request (`shdn 1`) raises a low-battery alert once.

## Wire protocol

The gateway speaks emu-serial-bridge's battery protocol:

```
BAT?  ->  BAT <pct> <CHG|DIS|UNK> <min> <watts> <chg> <shdn>
          e.g. BAT 81 CHG 332 9.1 1 0
```

`CHG`/`DIS` = external power present / on battery; `min` is the estimated
runtime (`-1` unavailable); `watts` is the output power as a decimal (`-1`
unavailable); `chg` = battery actively charging; `shdn` = the UPS has
requested a low-battery shutdown. Older bridges that send only the first two
fields still work. Also used: `BRI?` → `BRIGHT <pct>`, `BRIGHT <pct>` →
`OK BRIGHT <pct>`, `AUTO?`/`AUTO n` → `AUTO <0|1>`. The gateway's interfaces
for other applications are in `docs/apple-events.md`.

## Diagnostics

The gateway appends a log to `Emu Serial Gateway Log` in the Preferences folder
(open with SimpleText) and to `/tmp/emu-serial-gateway.log` on the host through
the emulator's Unix volume: start-up, every serial driver call with its
result, every transaction during the first two minutes and every failure
afterwards, and each Apple Event relay. `tail -F /tmp/emu-serial-gateway.log`
while the emulator boots answers "did the gateway launch" and "did the bridge
reply" directly. Fatal start-up conditions and a port that cannot be opened
raise a Notification Manager alert. The gateway can be quit from the
Application menu.

## File locations

| | |
|---|---|
| Mac | `System Folder:Startup Items:Emu Serial Gateway` |
| | `System Folder:Extensions:Emu Power Manager` |
| | `System Folder:Control Strip Modules:Emu Battery`, `Emu Brightness` |
| | `System Folder:Control Panels:Brightness` (Apple's original in Control Panels (Disabled)) |
| | `System Folder:Preferences:Emu Serial Gateway Prefs`, `Emu Serial Gateway Log` |
| Host | `/tmp/emu-serial-gateway.log`; `~/emu-mac/` if staged over SSH |

## Uninstall

Remove the five files from the System Folder, move Apple's parked files back,
restart; the preferences file and log in the Preferences folder can be
thrown away.

## Building (development Mac)

Apple's resource forks are not part of this repository. Extract them from a
System 7.5.5 image you own, then build:

```bash
pip3 install machfs
python3 tools/extract_native.py /path/to/MacOS755.hda   # -> resources/native/*.rsrc
RETRO68=~/dev/Retro68-build ./build.sh                  # configure, build, assemble, floppy, package
```

Produces `dist/extfs` (SheepShaver Unix-volume layout), `dist/macbinary`,
`dist/floppy/` (raw `.img` and DiskCopy 4.2 `.image`), and
`emu-control-strip-<version>.tar.gz`. `build.sh` checks that the three
code resources export their uppercased Pascal entry symbols; `assemble.py`
validates every input (flat entry trampoline, file types, SIZE flags,
required Apple resources) before writing anything; `make_floppy.py` re-reads
the image with an independent HFS implementation and compares every fork.

Layout:

```
mac/common/      EmuShared.h  ControlStripGlue.h  MacUtil.h
mac/gateway/     gateway.c  gateway.r
mac/battery/     battery.c
mac/brightness/  brightness.c
mac/cdev/        brightness_cdev.c
mac/powermgr/    powermgr.c
tools/           macres.py  assemble.py  make_floppy.py  extract_native.py
docs/            apple-events.md (status block + Apple Event relay for other apps)
floppy/          Read Me and Developer Notes sources (incl. Power Manager notes)
resources/native extracted Apple resource forks (git-ignored)
install.sh       host-side staging (alternative to the floppy)
```

## License

MIT (see LICENSE). The Control Strip module artwork, the Brightness control
panel resources and its slider control definition are Apple's, from System
7.5.5; they are not in this repository and are not covered by the licence.
