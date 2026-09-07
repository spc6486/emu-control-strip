/*
 * EmuShared.h — shared definitions for the Emu Control Strip Mac OS components.
 *
 * The Emu Serial Gateway (a background-only application) owns the emulator's
 * serial port and publishes hardware status in a block allocated in the
 * system heap.  The block is found through a Gestalt selector so that the
 * Control Strip modules and the Brightness control panel can read it with a
 * single Gestalt call and no I/O of their own.
 *
 * Readers (modules, cdev) only READ status fields and WRITE request fields.
 * The gateway READS request fields and WRITES status fields.  Mac OS 7.5 is
 * cooperatively scheduled and none of this runs at interrupt time, so no
 * locking is required.
 *
 * Every consumer must validate the block with EmuValidStatus() before use:
 * the Gestalt selector could in theory be registered by something else, and
 * an older/newer gateway could publish a different layout.
 */

#ifndef EMU_SHARED_H
#define EMU_SHARED_H

#include <MacTypes.h>
#include <Gestalt.h>
#include <OSUtils.h>
#include <Events.h>

/* ---- Identity ------------------------------------------------------------ */

#define kEmuGestaltSelector   0x456D7547UL   /* 'EmuG' */
#define kEmuBlockSignature    0x456D7547UL   /* 'EmuG' */
#define kEmuBlockVersion      1

/* Creator codes (unique to this project) */
#define kEmuGatewayCreator    0x424D6777UL   /* 'EmGw' */
#define kEmuBatteryCreator    0x424D6274UL   /* 'EmBt' */
#define kEmuBrightnessCreator 0x424D6272UL   /* 'EmBr' */
#define kEmuCdevCreator       0x424D6270UL   /* 'EmBp' */

/* Gateway version published in the block (major*100 + minor*10 + patch) */
#define kEmuGatewayVersion    100

/* ---- Enumerations -------------------------------------------------------- */

enum {
    kEmuBatUnknown     = 0,
    kEmuBatDischarging = 1,
    kEmuBatCharging    = 2,
    kEmuBatFull        = 3
};

enum {
    kEmuSrcUnknown = 0,
    kEmuSrcAC      = 1,
    kEmuSrcBattery = 2
};

enum {
    kEmuSerialClosed = 0,
    kEmuSerialOpen   = 1,
    kEmuSerialError  = 2
};

/* ---- Staleness thresholds (ticks; 60 ticks = 1 second) ------------------- */

#define kEmuHeartbeatStaleTicks   (15L * 60L)   /* gateway considered gone after 15 s */
#define kEmuBatteryStaleTicks     (45L * 60L)   /* battery data considered unknown after 45 s */
#define kEmuBrightnessStaleTicks  (30L * 60L)

/* ---- The status block ---------------------------------------------------- */

typedef struct EmuStatus {
    unsigned long  signature;      /* kEmuBlockSignature */
    short          version;        /* kEmuBlockVersion */
    short          size;           /* sizeof(EmuStatus) */

    unsigned long  heartbeat;      /* TickCount() at the gateway's last loop pass */
    unsigned long  batUpdated;     /* TickCount() of last good BAT reply, 0 = never */
    unsigned long  briUpdated;     /* TickCount() of last good BRI reply, 0 = never */

    /* Battery (from the bridge's BAT? reply) */
    short          batPercent;     /* 0..100, or -1 unknown */
    short          batState;       /* kEmuBat* */
    short          powerSource;    /* kEmuSrc* */
    short          runtimeMin;     /* estimated minutes remaining, -1 unknown */
    short          wattsX10;       /* output power in tenths of a watt, -1 unknown */

    /* Brightness (from BRI? / AUTO? replies) */
    short          brightness;     /* 0..100, or -1 unknown */
    short          autoDim;        /* 0/1, or -1 unknown */

    /* Requests from readers to the gateway.  A reader stores a value and
       bumps reqCounter; the gateway sends the command and resets to -1. */
    short          reqBrightness;  /* 0..100, or -1 none pending */
    short          reqAutoDim;     /* 0/1, or -1 none pending */
    unsigned long  reqCounter;

    /* Gateway information */
    short          gatewayVersion; /* kEmuGatewayVersion */
    short          serialState;    /* kEmuSerial* */
    short          errorCount;     /* consecutive failed transactions */

    /* Brightness hotkey settings (edited by the control panel, persisted by
       the gateway).  shortcutMods is an Event Manager modifier mask
       (shiftKey | optionKey | controlKey). */
    short          shortcutMods;
    short          hotkeyEnabled;  /* 0/1 */
    short          prefsDirty;     /* set by cdev after changing prefs */

    short          shutdownRequested; /* 1 while the UPS reports a low-battery shutdown request */

    /* Power Manager emulation (Emu Power Manager extension).  The
       extension's _PowerMgrDispatch handler returns pmScaledInfo verbatim for
       GetScaledBatteryInfo: flags in the top byte (bit 7 battery installed,
       bit 6 charging, bit 5 charger connected), level 0..255 in the bottom
       byte.  0 = "no battery" (the menu bar clock draws its X frame). */
    unsigned long  pmScaledInfo;
    short          reqSleep;       /* set by the _Sleep handler (1 request, 2 demand); gateway clears it */
    short          pmInstalled;    /* low byte: kEmuPM* status code; high byte: kEmuPMDid* flags */
} EmuStatus;

/* pmInstalled status codes (low byte) */
enum {
    kEmuPMNotRun          = 0,   /* extension never ran (or block created by the gateway) */
    kEmuPMInstalled       = 1,
    kEmuPMRealPowerMgr    = 2,   /* Gestalt 'powr' already reports PMgrExists: left alone */
    kEmuPMDispatchTaken   = 3,   /* _PowerMgrDispatch already implemented by something else */
    kEmuPMNoMemory        = 4,
    kEmuPMGestaltFailed   = 5
};
/* pmInstalled flags (high byte) */
#define kEmuPMDidDispatch   0x0100
#define kEmuPMDidSleep      0x0200
#define kEmuPMDidPMgrOp     0x0400
#define kEmuPMDidPowerDisp  0x0800
#define kEmuPMDidReplace    0x1000   /* 'powr' existed and was merged, not created */
#define kEmuPMDidChain      0x2000   /* chained in front of an existing _PowerMgrDispatch */

/* Packed GetScaledBatteryInfo flag bits (Power Manager 2.0 BatteryInfo.flags << 24) */
#define kEmuPMBatteryInstalled  0x80000000UL
#define kEmuPMBatteryCharging   0x40000000UL
#define kEmuPMChargerConnected  0x20000000UL

/* ---- Brightness levels (Sound Volume style 0..7 popup) ------------------- */

#define kEmuLevelCount 8

/* Percentages sent to the host for popup levels 0..7 (10..100 %). */
static const short kEmuLevelPercent[kEmuLevelCount] = { 10, 23, 36, 49, 61, 74, 87, 100 };

/* ---- Inline helpers ------------------------------------------------------ */

/* Returns true if the block pointer looks like a block published by a
   compatible gateway. */
static Boolean EmuValidStatus(const EmuStatus *s)
{
    if (s == 0L) return false;
    if (s->signature != kEmuBlockSignature) return false;
    if (s->version != kEmuBlockVersion) return false;
    if (s->size != (short)sizeof(EmuStatus)) return false;
    return true;
}

/* Looks up the shared block.  Returns NULL if the gateway has never
   published one (or the published block is not compatible). */
static EmuStatus *EmuFindStatus(void)
{
    long response = 0;
    OSErr err = Gestalt((OSType)kEmuGestaltSelector, (void *)&response);
    if (err != noErr || response == 0) return 0L;
    if (!EmuValidStatus((const EmuStatus *)response)) return 0L;
    return (EmuStatus *)response;
}

/* True if the gateway process has updated its heartbeat recently. */
static Boolean EmuGatewayAlive(const EmuStatus *s)
{
    unsigned long now;
    if (s == 0L) return false;
    now = TickCount();
    if (s->heartbeat == 0) return false;
    if (now < s->heartbeat) return true;           /* tick counter wrapped */
    return (now - s->heartbeat) < (unsigned long)kEmuHeartbeatStaleTicks;
}

/* True if battery fields are usable (gateway alive and data fresh). */
static Boolean EmuBatteryValid(const EmuStatus *s)
{
    unsigned long now;
    if (!EmuGatewayAlive(s)) return false;
    if (s->batUpdated == 0) return false;
    if (s->batPercent < 0) return false;
    now = TickCount();
    if (now < s->batUpdated) return true;
    return (now - s->batUpdated) < (unsigned long)kEmuBatteryStaleTicks;
}

/* True if brightness fields are usable. */
static Boolean EmuBrightnessValid(const EmuStatus *s)
{
    unsigned long now;
    if (!EmuGatewayAlive(s)) return false;
    if (s->briUpdated == 0) return false;
    if (s->brightness < 0) return false;
    now = TickCount();
    if (now < s->briUpdated) return true;
    return (now - s->briUpdated) < (unsigned long)kEmuBrightnessStaleTicks;
}

/* Nearest popup level (0..7) for a brightness percentage. */
static short EmuPercentToLevel(short pct)
{
    short i, best = 0, bestDist = 32767;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    for (i = 0; i < kEmuLevelCount; i++) {
        short d = kEmuLevelPercent[i] - pct;
        if (d < 0) d = -d;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

/* Post a brightness request (0..100). */
static void EmuRequestBrightness(EmuStatus *s, short pct)
{
    if (s == 0L) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s->reqBrightness = pct;
    s->brightness = pct;          /* optimistic: reflect immediately */
    s->reqCounter++;
}

/* Post an auto-dim request (0/1). */
static void EmuRequestAutoDim(EmuStatus *s, short enabled)
{
    if (s == 0L) return;
    s->reqAutoDim = enabled ? 1 : 0;
    s->autoDim = s->reqAutoDim;
    s->reqCounter++;
}

#endif /* EMU_SHARED_H */
