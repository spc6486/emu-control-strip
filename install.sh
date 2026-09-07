#!/usr/bin/env bash
# Emu Control Strip — host-side installer (stages the Mac files on the extfs volume)
#
#   ./install.sh              stage the Mac files on the extfs volume
#   ./install.sh --uninstall  remove the staged Mac files
#
# The battery protocol (BAT <pct> <CHG|DIS|UNK> <min> <watts> <chg> <shdn>) is
# provided by emu-serial-bridge itself; this package ships no handler.
#
# Mac files are staged in $STAGE_DIR (default ~/emu-mac), which the
# emulator exposes on its "Unix" volume with resource forks and Finder info
# intact (SheepShaver extfs .rsrc/.finf helper files).  The Mac OS side of
# the installation is a drag in the Finder — see the printed instructions.
#
# Every change made by this script is reversible with --uninstall.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE_DIR="${STAGE_DIR:-$HOME/emu-mac}"
DIST="$SCRIPT_DIR/dist/extfs"

ok()   { printf '\033[32m[ok]\033[0m %s\n' "$*"; }
info() { printf '\033[36m[..]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[!!]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[xx]\033[0m %s\n' "$*" >&2; exit 1; }

MODE="${1:-all}"

stage_mac_files() {
    [[ -d "$DIST" ]] || die "dist/extfs not found next to this script — run build.sh on the Mac first"
    for f in "Emu Serial Gateway" "Emu Power Manager" "Emu Battery" "Emu Brightness" "Brightness"; do
        [[ -f "$DIST/$f" ]]        || die "missing $DIST/$f"
        [[ -f "$DIST/.rsrc/$f" ]]  || die "missing resource fork for $f"
        [[ -f "$DIST/.finf/$f" ]]  || die "missing Finder info for $f"
        [[ $(stat -c %s "$DIST/.finf/$f") -eq 32 ]] || die "Finder info for $f is not 32 bytes"
    done
    mkdir -p "$STAGE_DIR/.rsrc" "$STAGE_DIR/.finf"
    cp "$DIST"/"Emu Serial Gateway" "$DIST"/"Emu Power Manager" "$DIST"/"Emu Battery" "$DIST"/"Emu Brightness" "$DIST"/"Brightness" "$STAGE_DIR/"
    cp "$DIST"/.rsrc/* "$STAGE_DIR/.rsrc/"
    cp "$DIST"/.finf/* "$STAGE_DIR/.finf/"
    cp "$SCRIPT_DIR/VERSION" "$STAGE_DIR/VERSION.txt" 2>/dev/null || true
    ok "staged Mac files in $STAGE_DIR"
    cat <<EOF

Next, inside the emulator (the Unix volume shows this directory as
  Unix:${STAGE_DIR#/}  with ':' in place of '/'):

  1. Drag  Emu Serial Gateway        -> System Folder:Startup Items
  2. Drag  Emu Power Manager  -> System Folder:Extensions
  3. Drag  Emu Battery        -> System Folder:Control Strip Modules
  4. Drag  Emu Brightness     -> System Folder:Control Strip Modules
  5. Move  Apple's Brightness control panel to Control Panels (Disabled), then
           drag Brightness           -> System Folder:Control Panels
  6. Move  Apple's Battery Monitor, Sleep Now, Power Settings and HD Spin Down
           out of Control Strip Modules, and PowerBook / PowerBook Setup out of
           Control Panels (they wake up once a Power Manager is advertised)
  7. Restart.  Apple's battery appears next to the menu bar clock by itself.

Check: the Control Strip shows a battery tile and a sun tile; the Brightness
control panel opens with the slider active.  Hold the pointer over a tile
with Balloon Help on to see the gateway status.
EOF
}

uninstall_mac_files() {
    if [[ -d "$STAGE_DIR" ]]; then
        rm -rf "$STAGE_DIR"
        ok "removed $STAGE_DIR (remove the copies inside the System Folder from the Finder)"
    fi
}

case "$MODE" in
    all|--mac-only) stage_mac_files ;;
    --uninstall)    uninstall_mac_files ;;
    *) die "usage: $0 [--uninstall]" ;;
esac
