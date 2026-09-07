# Changelog

All notable changes to emu-control-strip.

## [1.0.0] — 2026-09-07

First release.

### Added
- **Emu Serial Gateway**: application launched from Startup Items; owns
  the modem port, polls emu-serial-bridge with fully asynchronous serial
  I/O, publishes an `EmuStatus` block via Gestalt `'EmuG'`, relays Apple
  Events (`'EmuG'/'serl'`, `'misc'/'eval'`, `'misc'/'dosc'`) for other
  applications, logs to the Preferences folder and to the host through the
  emulator's Unix volume, and raises Notification Manager alerts for fatal
  conditions and UPS shutdown requests.
- **Emu Battery** and **Emu Brightness** Control Strip modules, built on
  the artwork, layouts and helper calls recovered from System 7.5.5's
  Battery Monitor and Sound Volume modules.
- **Brightness** control panel: Apple's panel resources (DITL, slider
  CDEF, pictures, help) with new code driving the host backlight.
- **Emu Power Manager** extension: presents the part of the PowerBook Power
  Manager that Apple's menu bar clock reads (`Gestalt 'powr'`,
  `_PowerMgrDispatch` selectors 12 and 26, chained in front of an existing
  2.0 dispatch where the ROM has one), so Date & Time's "Show the battery
  level" works and Apple's own 17 battery frames are drawn from the host's
  charge.
- Floppy distribution (`tools/make_floppy.py`): 1.44 MB HFS image (raw and
  DiskCopy 4.2) with Read Me and Developer Notes; `tools/extract_native.py`
  extracts Apple's resource forks from the builder's own System 7.5.5
  image (not distributed).

### Development notes
Findings from bringing this up under SheepShaver on a Power Mac 8600 ROM,
recorded because they are not obvious from documentation:
- Apple's Battery Monitor repaints itself inside `sdevPeriodicTickle`;
  result bit 2 is only the help-state flag, not a redraw request.
- Apple's slider CDEF draws nothing until the control is shown; the `CNTL`
  resource defines it invisible and `initDev` must call `ShowControl`.
- Multiversal Interfaces (as of 2026) emit the `_Gestalt` trap word for
  `NewGestalt`; the gateway and extension declare `_NewGestalt` (0xA3AD)
  and `_ReplaceGestalt` (0xA5AD) themselves.
- SheepShaver services serial I/O with blocking host calls on a PTY and
  registers its serial port as a native driver; the gateway therefore uses
  only asynchronous reads/writes (with stuck-request detection) and
  immediate control/status calls.
- The serial driver's open never completed for a background-only `appe`
  launched from the Extensions folder, while identical code launched by the
  Finder worked; the gateway is a normal application in Startup Items that
  hands the foreground back to the Finder.
- PCI Power Macs report `Gestalt('powr') = $10` (Power Manager 2.0
  dispatch, used by Energy Saver, with `_Sleep` and `_PowerDispatch`
  implemented) without the PMgrExists bit the menu bar clock requires.
