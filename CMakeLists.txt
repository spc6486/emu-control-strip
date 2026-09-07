# Emu Control Strip — Mac OS side build (Retro68, 68k)
#
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=$RETRO68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   make
#
# Produces:
#   build/battery.flt           flat 'sdev' code  -> Emu Battery
#   build/brightness.flt        flat 'sdev' code  -> Emu Brightness
#   build/brightness_cdev.flt   flat 'cdev' code  -> Brightness (control panel)
#   build/powermgr.flt          flat 'INIT' code  -> Emu Power Manager (extension)
#   build/EmuSerialGateway.bin    MacBinary 'APPL'  -> Emu Serial Gateway
# tools/assemble.py then combines these with Apple's resources into
# deployable files (see build.sh).

cmake_minimum_required(VERSION 3.9)
project(EmuControlStrip C)

include_directories(${CMAKE_SOURCE_DIR}/mac/common)

set(BM_CODE_RESOURCE_CFLAGS -ffunction-sections -fdata-sections -Os)

# ---- helper: flat code resource ------------------------------------------
# Pascal entry points get UPPERCASED link names under Retro68's m68k target.
function(emu_add_code_resource target sources entry)
    add_executable(${target} ${sources})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${target}.flt"
        COMPILE_OPTIONS "${BM_CODE_RESOURCE_CFLAGS}"
        LINK_FLAGS "-Wl,--mac-flat -Wl,-e${entry} -Wl,-gc-sections")
endfunction()

emu_add_code_resource(battery         mac/battery/battery.c              EMUBATTERYMODULE)
emu_add_code_resource(brightness      mac/brightness/brightness.c        EMUBRIGHTNESSMODULE)
emu_add_code_resource(brightness_cdev mac/cdev/brightness_cdev.c         EMUBRIGHTNESSCDEV)
emu_add_code_resource(powermgr        "mac/powermgr/powermgr.c;mac/powermgr/showinit.c" _start)

# ---- gateway application -------------------------------------------------
add_application(EmuSerialGateway
    TYPE "APPL"
    CREATOR "EmGw"
    mac/gateway/gateway.c
    mac/gateway/gateway.r)
set_target_properties(EmuSerialGateway PROPERTIES
    COMPILE_OPTIONS "-Os"
    LINK_FLAGS "-Wl,-gc-sections")

