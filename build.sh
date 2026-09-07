#!/usr/bin/env bash
# Emu Control Strip — build on the development Mac with Retro68.
#
#   ./build.sh            configure (if needed), build, assemble, package
#   ./build.sh --clean    remove build/ and dist/ first
#
# Environment:
#   RETRO68   Retro68 build directory (default ~/dev/Retro68-build)
#
# Result: dist/extfs (SheepShaver Unix-volume layout), dist/macbinary, and
# emu-control-strip-<version>.tar.gz containing dist/, install.sh and docs for scp to
# the emulator host.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

RETRO68="${RETRO68:-$HOME/dev/Retro68-build}"
TOOLCHAIN="$RETRO68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"
VERSION="$(cat VERSION)"

ok()   { printf '\033[32m[ok]\033[0m %s\n' "$*"; }
info() { printf '\033[36m[..]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[xx]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf build dist
    ok "cleaned"
fi

[[ -f "$TOOLCHAIN" ]] || die "Retro68 toolchain file not found: $TOOLCHAIN (set RETRO68)"
command -v cmake >/dev/null || die "cmake not found"
command -v python3 >/dev/null || die "python3 not found"
for f in Battery_Monitor.rsrc Sound_Volume.rsrc Brightness.rsrc; do
    [[ -f "resources/native/$f" ]] || die "resources/native/$f missing — run: python3 tools/extract_native.py /path/to/your/System-7.5.5-image"
done

info "configuring"
mkdir -p build
( cd build && cmake .. -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release >/dev/null )

info "building Mac-side code"
( cd build && make )

# Entry symbols must exist with their uppercased Pascal link names.
NM="$RETRO68/toolchain/bin/m68k-apple-macos-nm"
for pair in "battery.flt:EMUBATTERYMODULE" "brightness.flt:EMUBRIGHTNESSMODULE" "brightness_cdev.flt:EMUBRIGHTNESSCDEV"; do
    f="${pair%%:*}"; sym="${pair##*:}"
    [[ -f "build/$f" ]] || die "build/$f was not produced"
    # The .flt is the flat binary; the ELF with symbols sits next to it as <name>.flt.gdb or in CMakeFiles.
    elf="$(find build/CMakeFiles/${f%.flt}.dir -name '*.obj' 2>/dev/null | head -1 || true)"
    if [[ -n "$elf" && -x "$NM" ]]; then
        "$NM" "$elf" | grep -q " T $sym\$" || die "$f: entry symbol $sym not found in $elf (pascal name mangling changed?)"
    fi
done
ok "code resources built"

info "assembling deliverables"
rm -rf dist
python3 tools/assemble.py --build build --native resources/native --out dist --version "$VERSION"

info "building the floppy image"
TARBALL="emu-control-strip-$VERSION.tar.gz"
python3 tools/make_floppy.py --dist dist --texts floppy --header mac/common/EmuShared.h --out dist/floppy --version "$VERSION" --date "$(date +%Y-%m-%d)" --hfsutils "$RETRO68/toolchain/bin"
ok "floppy image built"

info "packaging"
tar -czf "$TARBALL" dist docs floppy install.sh VERSION README.md CHANGELOG.md
ok "wrote $TARBALL"
cat <<EOF

Deploy:
  scp $TARBALL <user>@<host>:~
  ssh <user>@<host>
  tar -xzf $TARBALL && ./install.sh
EOF
