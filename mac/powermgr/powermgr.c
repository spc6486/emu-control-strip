/*
 * powermgr.c — "Emu Power Manager" system extension ('INIT').
 *
 * Presents enough of the PowerBook Power Manager interface for Apple's own
 * software to show battery state on an emulated Macintosh.  The first consumer is the
 * System 7.5 menu bar clock (Date & Time control panel, "Show the battery
 * level"): at INIT time it checks Gestalt('powr') for bit 0 (Power Manager
 * exists) and bit 4 (Power Manager 2.0 dispatch), then reads the battery
 * with _PowerMgrDispatch selectors 26 (BatteryCount) and 12
 * (GetScaledBatteryInfo, packed in D0), and clicking the icon calls _Sleep.
 *
 * Because the clock decides once, at boot, this must run before the Date &
 * Time INIT: it lives in the Extensions folder, which loads before the
 * Control Panels folder.
 *
 * What is installed (all in the system heap, address-patched 68000 stubs so
 * nothing depends on this INIT's code staying resident):
 *   - the EmuStatus block and its 'EmuG' Gestalt selector, if not
 *     already present (the gateway later finds and fills it);
 *   - Gestalt 'powr' = 0x11 (PMgrExists | PMgrDispatchExists);
 *   - _PowerMgrDispatch (A09E): 12 GetScaledBatteryInfo -> block->pmScaledInfo,
 *     26 BatteryCount -> 1.  Where the machine already has a 2.0 dispatch
 *     (PCI Power Macs: Energy Saver) ours chains in front of it and passes
 *     every other selector through; otherwise ours answers 0 PMSelectorCount
 *     -> 27, 1 PMFeatures -> 0 and 0 for the rest;
 *   - _Sleep (A08A): stores the sleep mode in block->reqSleep for the
 *     gateway (SleepQInstall/SleepQRemove variants are ignored);
 *   - _PMgrOp (A085) -> pmBusyErr, _PowerDispatch (A09F) -> 0, so that a
 *     caller which trusts Gestalt never hits an unimplemented trap.
 *
 * A machine whose 'powr' already reports a Power Manager is left alone; an
 * existing 'powr' without that bit (Energy Saver, ROM) is merged with ours.
 * Traps other than _PowerMgrDispatch are installed only where nothing else
 * has implemented them.  What happened is recorded in the block's
 * pmInstalled field (kEmuPM* codes) and reported by the gateway's log; a
 * startup icon is drawn in the INIT parade.
 *
 * Build: Retro68, -Wl,--mac-flat (entry _start).
 */

#include <MacTypes.h>
#include <Memory.h>
#include <Gestalt.h>
#include <OSUtils.h>
#include <Traps.h>
#include <Retro68Runtime.h>

#include "EmuShared.h"
#include "MacUtil.h"
#include "showinit.h"

#define kTrapPowerMgrDispatch  0xA09E
#define kTrapPowerDispatch     0xA09F
#define kTrapPMgrOp            0xA085
#define kTrapSleep             0xA08A
#define kTrapUnimplemented     0xA89F

#define kGestaltPowerMgrAttr   0x706F7772UL   /* 'powr' */
#define kPowerMgrAttrValue     0x00000011UL   /* gestaltPMgrExists | gestaltPMgrDispatchExists */

/* ---- 68000 stubs ---------------------------------------------------------- */

/* Gestalt selector function (Pascal: OSErr fn(OSType selector, long *response)):
   MOVEA.L 4(SP),A0 ; MOVE.L #value,(A0) ; MOVEA.L (SP)+,A1 ; ADDQ.L #8,SP ; CLR.W (SP) ; JMP (A1) */
static const unsigned char kGestaltStub[18] = {
    0x20, 0x6F, 0x00, 0x04,
    0x20, 0xBC, 0x00, 0x00, 0x00, 0x00,          /* value patched at offset 6 */
    0x22, 0x5F, 0x50, 0x8F, 0x42, 0x57, 0x4E, 0xD1
};
#define kGestaltStubValueOffset 6

/* _PowerMgrDispatch handler.  Entry: D0.L = (battery index << 16) | selector.
   Result in D0.L.  A0 preserved (saved around the one load that uses it). */
static const unsigned char kPowerMgrDispatchStub[52] = {
    0xB0, 0x7C, 0x00, 0x0C,        /* 00: cmp.w  #12,d0        GetScaledBatteryInfo */
    0x67, 0x20,                    /* 04: beq.s  info (26)      */
    0xB0, 0x7C, 0x00, 0x1A,        /* 06: cmp.w  #26,d0        BatteryCount */
    0x67, 0x0E,                    /* 0A: beq.s  count (1A)     */
    0x4A, 0x40,                    /* 0C: tst.w  d0            PMSelectorCount */
    0x67, 0x0E,                    /* 0E: beq.s  selcount (1E)  */
    0xB0, 0x7C, 0x00, 0x01,        /* 10: cmp.w  #1,d0         PMFeatures */
    0x67, 0x0C,                    /* 14: beq.s  features (22)  */
    0x70, 0x00,                    /* 16: moveq  #0,d0         unknown selector */
    0x4E, 0x75,                    /* 18: rts                   */
    0x70, 0x01,                    /* 1A: moveq  #1,d0         one battery */
    0x4E, 0x75,                    /* 1C: rts                   */
    0x70, 0x1B,                    /* 1E: moveq  #27,d0        selectors 0..26 */
    0x4E, 0x75,                    /* 20: rts                   */
    0x70, 0x00,                    /* 22: moveq  #0,d0         no optional features */
    0x4E, 0x75,                    /* 24: rts                   */
    0x2F, 0x08,                    /* 26: move.l a0,-(sp)       */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x00, /* 28: movea.l #addr,a0   patched at offset 0x2A */
    0x20, 0x10,                    /* 2E: move.l (a0),d0        pmScaledInfo */
    0x20, 0x5F,                    /* 30: movea.l (sp)+,a0      */
    0x4E, 0x75                     /* 32: rts                   */
};
#define kPowerMgrDispatchAddrOffset 0x2A

/* Chained _PowerMgrDispatch handler, used when the machine already has a
   Power Manager 2.0 dispatch (PCI Power Macs: Energy Saver's sleep and
   spin-down live there).  Selectors 12 and 26 are answered here; every
   other selector jumps to the previous handler, whose RTS returns to the
   trap dispatcher directly. */
static const unsigned char kPowerMgrDispatchChainStub[38] = {
    0xB0, 0x7C, 0x00, 0x0C,        /* 00: cmp.w  #12,d0        GetScaledBatteryInfo */
    0x67, 0x12,                    /* 04: beq.s  info (18)      */
    0xB0, 0x7C, 0x00, 0x1A,        /* 06: cmp.w  #26,d0        BatteryCount */
    0x67, 0x08,                    /* 0A: beq.s  count (14)     */
    0x4E, 0xF9, 0x00, 0x00, 0x00, 0x00, /* 0C: jmp    previous     patched at offset 0x0E */
    0x4E, 0x71,                    /* 12: nop                   */
    0x70, 0x01,                    /* 14: moveq  #1,d0         one battery */
    0x4E, 0x75,                    /* 16: rts                   */
    0x2F, 0x08,                    /* 18: move.l a0,-(sp)       */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x00, /* 1A: movea.l #addr,a0   patched at offset 0x1C */
    0x20, 0x10,                    /* 20: move.l (a0),d0        pmScaledInfo */
    0x20, 0x5F,                    /* 22: movea.l (sp)+,a0      */
    0x4E, 0x75                     /* 24: rts                   */
};
#define kChainPrevOffset 0x0E
#define kChainAddrOffset 0x1C

/* _Sleep handler.  Entry: D0 = sleep mode; D1 = trap word (bits 9/10 set for
   SleepQInstall / SleepQRemove, which are ignored).  Returns D0 = 0. */
static const unsigned char kSleepStub[28] = {
    0x08, 0x01, 0x00, 0x09,        /* 00: btst   #9,d1          */
    0x66, 0x12,                    /* 04: bne.s  done (18)      */
    0x08, 0x01, 0x00, 0x0A,        /* 06: btst   #10,d1         */
    0x66, 0x0C,                    /* 0A: bne.s  done (18)      */
    0x2F, 0x08,                    /* 0C: move.l a0,-(sp)       */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x00, /* 0E: movea.l #addr,a0   patched at offset 0x10 */
    0x30, 0x80,                    /* 14: move.w d0,(a0)        reqSleep = mode */
    0x20, 0x5F,                    /* 16: movea.l (sp)+,a0      */
    0x70, 0x00,                    /* 18: moveq  #0,d0          */
    0x4E, 0x75                     /* 1A: rts                   */
};
#define kSleepAddrOffset 0x10

/* _PMgrOp: moveq #-13,d0 (pmBusyErr); rts */
static const unsigned char kPMgrOpStub[4] = { 0x70, 0xF3, 0x4E, 0x75 };
/* _PowerDispatch: moveq #0,d0; rts */
static const unsigned char kPowerDispatchStub[4] = { 0x70, 0x00, 0x4E, 0x75 };

/* ---- helpers ------------------------------------------------------------- */

static void PatchLong(Ptr p, short offset, unsigned long value)
{
    p[offset + 0] = (char)(value >> 24);
    p[offset + 1] = (char)(value >> 16);
    p[offset + 2] = (char)(value >> 8);
    p[offset + 3] = (char)(value);
}

static Ptr InstallStub(const unsigned char *code, short len, short patchOffset, unsigned long value)
{
    Ptr p = NewPtrSys(len);
    if (p == 0L) return 0L;
    BlockMoveData((Ptr)code, p, len);
    if (patchOffset >= 0) PatchLong(p, patchOffset, value);
    return p;
}

static Boolean TrapIsUnimplemented(unsigned short trapWord)
{
    return GetOSTrapAddress(trapWord) == GetToolTrapAddress(kTrapUnimplemented);
}

/* Find the shared block, creating and publishing it if this is the first
   Emu Control Strip component to run.  Returns NULL only on allocation failure. */
static EmuStatus *ObtainStatusBlock(void)
{
    long response = 0;
    EmuStatus *s;
    Ptr stub;

    if (Gestalt((OSType)kEmuGestaltSelector, (void *)&response) == noErr && response != 0) {
        s = (EmuStatus *)response;
        return EmuValidStatus(s) ? s : 0L;
    }
    s = (EmuStatus *)NewPtrSysClear(sizeof(EmuStatus));
    if (s == 0L) return 0L;
    s->signature = kEmuBlockSignature;
    s->version = kEmuBlockVersion;
    s->size = (short)sizeof(EmuStatus);
    s->batPercent = -1;
    s->runtimeMin = -1;
    s->wattsX10 = -1;
    s->brightness = -1;
    s->autoDim = -1;
    s->reqBrightness = -1;
    s->reqAutoDim = -1;
    stub = InstallStub(kGestaltStub, sizeof(kGestaltStub), kGestaltStubValueOffset, (unsigned long)s);
    if (stub == 0L) { DisposePtr((Ptr)s); return 0L; }
    if (EmuNewGestalt((OSType)kEmuGestaltSelector, (void *)stub) != noErr) {
        DisposePtr(stub);
        DisposePtr((Ptr)s);
        return 0L;
    }
    return s;
}

/* ---- entry --------------------------------------------------------------- */

#define kInitIconID 128

static void UndoTrap(unsigned short trapWord, Boolean did, ProcPtr previous)
{
    if (did) SetOSTrapAddress(previous, trapWord);
}

void _start(void)
{
    EmuStatus *block;
    long existing = 0;
    Boolean powrExists;
    Ptr dispatch = 0L, sleep = 0L, pmgrOp = 0L, powerDispatch = 0L, powrStub = 0L;
    ProcPtr prevDispatch, unimplemented;
    short did = 0;
    OSErr err;

    RETRO68_RELOCATE();

    EmuShowInitIcon(kInitIconID);

    block = ObtainStatusBlock();
    if (block == 0L) return;

    /* Merge with an existing 'powr' selector; a machine that already reports
       a Power Manager (bit 0) is left alone. */
    powrExists = (Gestalt((OSType)kGestaltPowerMgrAttr, (void *)&existing) == noErr);
    if (powrExists && (existing & 1)) { block->pmInstalled = kEmuPMRealPowerMgr; return; }
    if (!powrExists) existing = 0;

    /* GetScaledBatteryInfo and BatteryCount must be ours.  If a Power Manager
       2.0 dispatch already exists (Energy Saver on PCI Power Macs) chain in
       front of it; otherwise provide the whole dispatch.  The other traps
       are installed only where nothing else has implemented them. */
    if (TrapIsUnimplemented(kTrapPowerMgrDispatch)) {
        dispatch = InstallStub(kPowerMgrDispatchStub, sizeof(kPowerMgrDispatchStub),
                               kPowerMgrDispatchAddrOffset, (unsigned long)&block->pmScaledInfo);
    } else {
        dispatch = InstallStub(kPowerMgrDispatchChainStub, sizeof(kPowerMgrDispatchChainStub),
                               kChainAddrOffset, (unsigned long)&block->pmScaledInfo);
        if (dispatch) PatchLong(dispatch, kChainPrevOffset, (unsigned long)GetOSTrapAddress(kTrapPowerMgrDispatch));
        did |= kEmuPMDidChain;
    }
    powrStub = InstallStub(kGestaltStub, sizeof(kGestaltStub), kGestaltStubValueOffset,
                           (unsigned long)(existing | kPowerMgrAttrValue));
    if (TrapIsUnimplemented(kTrapSleep))
        sleep = InstallStub(kSleepStub, sizeof(kSleepStub), kSleepAddrOffset, (unsigned long)&block->reqSleep);
    if (TrapIsUnimplemented(kTrapPMgrOp))
        pmgrOp = InstallStub(kPMgrOpStub, sizeof(kPMgrOpStub), -1, 0);
    if (TrapIsUnimplemented(kTrapPowerDispatch))
        powerDispatch = InstallStub(kPowerDispatchStub, sizeof(kPowerDispatchStub), -1, 0);
    if (dispatch == 0L || powrStub == 0L) {
        if (dispatch) DisposePtr(dispatch);
        if (powrStub) DisposePtr(powrStub);
        if (sleep) DisposePtr(sleep);
        if (pmgrOp) DisposePtr(pmgrOp);
        if (powerDispatch) DisposePtr(powerDispatch);
        block->pmInstalled = kEmuPMNoMemory;
        return;
    }
    if (TrapAvailable(0xA0BD)) FlushCodeCache();

    /* Traps first, Gestalt last: nothing can see 'powr' before the traps exist. */
    unimplemented = GetToolTrapAddress(kTrapUnimplemented);
    prevDispatch = GetOSTrapAddress(kTrapPowerMgrDispatch);
    SetOSTrapAddress((ProcPtr)dispatch, kTrapPowerMgrDispatch);      did |= kEmuPMDidDispatch;
    if (sleep)         { SetOSTrapAddress((ProcPtr)sleep, kTrapSleep);                  did |= kEmuPMDidSleep; }
    if (pmgrOp)        { SetOSTrapAddress((ProcPtr)pmgrOp, kTrapPMgrOp);                did |= kEmuPMDidPMgrOp; }
    if (powerDispatch) { SetOSTrapAddress((ProcPtr)powerDispatch, kTrapPowerDispatch);  did |= kEmuPMDidPowerDisp; }

    if (powrExists) { err = EmuReplaceGestalt((OSType)kGestaltPowerMgrAttr, (void *)powrStub); did |= kEmuPMDidReplace; }
    else            { err = EmuNewGestalt((OSType)kGestaltPowerMgrAttr, (void *)powrStub); }
    if (err != noErr) {
        UndoTrap(kTrapPowerMgrDispatch, true, prevDispatch);
        UndoTrap(kTrapSleep, sleep != 0L, unimplemented);
        UndoTrap(kTrapPMgrOp, pmgrOp != 0L, unimplemented);
        UndoTrap(kTrapPowerDispatch, powerDispatch != 0L, unimplemented);
        block->pmInstalled = kEmuPMGestaltFailed;
        return;
    }
    block->pmScaledInfo = 0;         /* no data yet: the clock shows its X frame */
    block->reqSleep = 0;
    block->pmInstalled = (short)(kEmuPMInstalled | did);
}
