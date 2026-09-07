/*
 * gateway.c — "Emu Serial Gateway", an application launched from Startup Items.
 *
 * It is an ordinary application (not a background-only 'appe'): under
 * SheepShaver the serial driver's open never completes for a faceless
 * process launched from the Extensions folder, while the identical code
 * works when launched by the Finder.  The gateway therefore starts as a
 * normal application, shows only an Apple menu and File > Quit, and hands
 * the foreground back to the Finder as soon as it is running.
 *
 * Responsibilities
 *   1. Own the modem serial port (.AIn/.AOut -> SheepShaver seriala ->
 *      emu-serial-bridge on the host).  Nothing else on the Mac side touches
 *      the port while the gateway runs.
 *   2. Poll the bridge (BAT?, BRI?, AUTO?) and publish the results in a
 *      EmuStatus block in the system heap, reachable through the
 *      'EmuG' Gestalt selector (see EmuShared.h).
 *   3. Execute requests posted in the block by the Control Strip modules
 *      and the Brightness control panel (BRIGHT n, AUTO n).
 *   4. Apple Event relay: any application can send a bridge command
 *      through the gateway and receive the reply text (see
 *      docs/apple-events.md).  Handled events: 'misc'/'eval' (HyperTalk
 *      request), 'misc'/'dosc' (AppleScript do script), and the gateway's
 *      own 'EmuG'/'serl'.  The reply's direct parameter is the bridge's
 *      reply text, lines separated by return characters.
 *      Gateway meta commands: GATEWAY STATUS | PAUSE | RESUME | VERSION.
 *   5. Persist the brightness-hotkey preference edited by the control panel
 *      ("Emu Serial Gateway Prefs" in the Preferences folder).
 *
 * Serial transactions are a non-blocking state machine serviced from the
 * event loop, so a silent bridge never stalls the (cooperatively
 * multitasked) system.  Only the Apple Event relay waits synchronously,
 * with a hard bound, because its caller is blocked waiting for the reply.
 *
 * The Gestalt selector function is an 18-byte 68k stub placed in the system
 * heap, so a Gestalt call can never dangle into this application's heap
 * even after the gateway quits.  A relaunched gateway reuses the block.
 *
 * Wire protocol (emu-serial-bridge, CR-terminated lines):
 *   BAT?          -> BAT <pct> <CHG|DIS|UNK> [<min|-1> <watts|-1> <chg 0|1> <shdn 0|1>]
 *                    CHG/DIS = plugged in / on battery; watts is a decimal;
 *                    older bridges send only the first two fields
 *   BRI?          -> BRIGHT <pct>
 *   BRIGHT <pct>  -> OK BRIGHT <pct>
 *   AUTO?         -> AUTO <0|1>
 *   AUTO <0|1>    -> OK AUTO <0|1>
 *   errors        -> ERR ...
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Events.h>
#include <Processes.h>
#include <AppleEvents.h>
#include <Gestalt.h>
#include <Serial.h>
#include <Devices.h>
#include <Files.h>
#if defined(__has_include)
#  if __has_include(<Folders.h>)
#    include <Folders.h>     /* FindFolder lives here in Apple's Universal Interfaces */
#  endif
#endif
#include <Errors.h>
#include <SegLoad.h>

#include "EmuShared.h"
#include "MacUtil.h"

/* ---- Tunables ------------------------------------------------------------ */
#define kInputBufferSize      4096
#define kLineMax              512
#define kReplyMax             3072
#define kSleepIdleTicks       6          /* WaitNextEvent sleep while idle: 0.1 s */
#define kSleepBusyTicks       1          /* while a transaction is in flight */
#define kBatPollTicks         (10L * 60L)
#define kBriPollTicks         (3L * 60L)
#define kAutoPollTicks        (60L * 60L)
#define kSerialRetryTicks     (10L * 60L)
#define kPollFirstByteTicks   (2L * 60L)  /* bridge normally answers within 100 ms */
#define kPollQuietTicks       10
#define kPollHardTicks        (3L * 60L)
#define kRelayFirstByteTicks  (6L * 60L)  /* HA commands include an HTTP round trip */
#define kRelayQuietTicks      20
#define kRelayHardTicks       (12L * 60L)
#define kMaxErrorsBeforeUnknown 3
#define kBackoffAfterErrors   3           /* consecutive failures before slowing polls */
#define kBackoffMultiplier    5

#ifndef fsRdWrPerm
#  define fsRdWrPerm 3
#endif
#ifndef smSystemScript
#  define smSystemScript (-1)
#endif

/* ---- Globals ------------------------------------------------------------- */
static EmuStatus *gStatus = 0L;
static Boolean gAlreadyRunning = false;
static Boolean gSentToBack = false;
static short   gInRef = 0, gOutRef = 0;
static Ptr     gInBuf = 0L;          /* SerSetBuf buffer (advisory) */
static Boolean gSerialOK = false;
static Boolean gQuit = false;
static Boolean gPaused = false;
static unsigned long gNextBat = 0, gNextBri = 0, gNextAuto = 0, gNextSerialRetry = 0;

/* Transaction state machine */
enum { txIdle = 0, txFlush, txSend, txSending, txWaiting };
enum { kindNone = 0, kindBat, kindBri, kindAuto, kindSetBri, kindSetAuto, kindSleep, kindRelay };

static short   gTxState = txIdle;
static short   gTxKind = kindNone;
static char    gRaw[kReplyMax];
static long    gRawLen = 0, gLineStart = 0, gLines = 0;
static Boolean gFirstLineSeen = false, gExpectMore = false;
static unsigned long gTxStart = 0, gTxLastByte = 0;
static long    gTxFirstTicks = 0, gTxQuietTicks = 0, gTxHardTicks = 0;
static char    gReply[kReplyMax];       /* completed reply, lines joined by '\r' */
static long    gReplyLen = 0;

/* ---- Preferences --------------------------------------------------------- */
#define kPrefsMagic   0x424D6770UL   /* 'BMgp' */
#define kPrefsVersion 1
typedef struct {
    unsigned long magic;
    short version;
    short shortcutMods;
    short hotkeyEnabled;
    short reserved;
} GatewayPrefs;

/* ========================================================================== */
/* Diagnostic log on the emulator's Unix volume (/tmp/emu-serial-gateway.log)   */
/* ========================================================================== */

#define kLogPathUnix  "Unix:tmp:emu-serial-gateway.log"   /* visible on the host through extfs */
#define kLogNamePrefs "Emu Serial Gateway Log"            /* on the boot disk, Preferences folder */
#define kLogMaxBytes  (64L * 1024L)

static Boolean gLogUnixAvailable = true;
static unsigned long gLogUnixRetryAt = 0;
static unsigned long gLogStartTicks = 0;

static void FormatLogLine(const char *text, char *line, short *outLen)
{
    unsigned long secs;
    DateTimeRec dt;
    short n = 0, i;
    GetDateTime(&secs);
    SecondsToDate(secs, &dt);      /* Secs2Date in Universal Interfaces */
    line[n++] = (char)('0' + dt.hour / 10);   line[n++] = (char)('0' + dt.hour % 10);   line[n++] = ':';
    line[n++] = (char)('0' + dt.minute / 10); line[n++] = (char)('0' + dt.minute % 10); line[n++] = ':';
    line[n++] = (char)('0' + dt.second / 10); line[n++] = (char)('0' + dt.second % 10); line[n++] = ' ';
    for (i = 0; text[i] != 0 && n < 316; i++)
        line[n++] = (text[i] == '\r') ? '|' : text[i];
    *outLen = n;
}

/* Append a line to the file described by spec (created if missing). */
static OSErr AppendToFile(FSSpec *spec, const char *line, short len, char lineEnd)
{
    OSErr err;
    short ref = 0;
    long count, eof = 0;
    err = FSpOpenDF(spec, fsRdWrPerm, &ref);
    if (err == fnfErr) {
        err = FSpCreate(spec, (OSType)0x74747874UL /* 'ttxt' */, (OSType)0x54455854UL /* 'TEXT' */, smSystemScript);
        if (err == noErr) err = FSpOpenDF(spec, fsRdWrPerm, &ref);
    }
    if (err != noErr) return err;
    if (GetEOF(ref, (void *)&eof) == noErr && eof > kLogMaxBytes) SetEOF(ref, 0);   /* keep it bounded */
    if (SetFPos(ref, fsFromLEOF, 0) == noErr) {
        count = len;
        FSWrite(ref, (void *)&count, (Ptr)line);
        count = 1;
        FSWrite(ref, (void *)&count, (Ptr)&lineEnd);
    }
    FSClose(ref);
    return noErr;
}

static FSSpec  gPrefsLogSpec;
static Boolean gPrefsLogSpecValid = false;

static void GatewayLog(const char *text)
{
    FSSpec spec;
    OSErr err;
    Str255 name;
    char line[320];
    short len = 0;
    short vRef = 0;
    long dirID = 0;

    FormatLogLine(text, line, &len);

    /* Unix volume first (host file, cheapest and visible from the host). */
    if (gLogUnixAvailable || TickCount() >= gLogUnixRetryAt) {
        gLogUnixAvailable = true;
        PStrFromC(name, kLogPathUnix);
        err = FSMakeFSSpec(0, 0, name, &spec);
        if ((err != noErr && err != fnfErr) || AppendToFile(&spec, line, len, '\n') != noErr) {
            gLogUnixAvailable = false;
            gLogUnixRetryAt = TickCount() + 60L * 60L;
        }
    }

    /* Boot disk: Preferences folder, Mac line endings (readable in SimpleText). */
    if (!gPrefsLogSpecValid) {
        if (FindFolder(kOnSystemDisk, kPreferencesFolderType, true, &vRef, (void *)&dirID) == noErr) {
            PStrFromC(name, kLogNamePrefs);
            err = FSMakeFSSpec(vRef, dirID, name, &gPrefsLogSpec);
            if (err == noErr || err == fnfErr) gPrefsLogSpecValid = true;
        }
    }
    if (gPrefsLogSpecValid) AppendToFile(&gPrefsLogSpec, line, len, '\r');
}

/* Notification Manager alert (the only UI a background-only app may use). */
static void Notify(const char *text)
{
    NMRec *nm = (NMRec *)NewPtrSysClear(sizeof(NMRec) + 256);
    StringPtr str;
    if (nm == 0L) return;
    str = (StringPtr)((Ptr)nm + sizeof(NMRec));
    PStrFromC(str, text);
    nm->qType = 8;                       /* nmType */
    nm->nmMark = 0;
    nm->nmIcon = 0L;
    nm->nmSound = (Handle)-1L;           /* system alert sound */
    nm->nmStr = str;
    nm->nmResp = (ProcPtr)-1L;           /* remove automatically after the user dismisses it */
    nm->nmRefCon = 0;
    NMInstall(nm);                       /* record intentionally leaks: it must outlive us */
}

/* printf-lite: text + optional number(s) */
static void LogNum(const char *text, long a)
{
    Str255 s; char c[300];
    PStrFromC(s, text); PStrAppendNum(s, a); PStrToC(s, c, sizeof(c)); GatewayLog(c);
}

static void LogNum2(const char *text, long a, const char *mid, long b)
{
    Str255 s, t; char c[300];
    PStrFromC(s, text); PStrAppendNum(s, a);
    PStrFromC(t, mid); PStrAppend(s, t); PStrAppendNum(s, b);
    PStrToC(s, c, sizeof(c)); GatewayLog(c);
}

static void PStrAppendHex(StringPtr dst, unsigned long v)
{
    const char *digits = "0123456789ABCDEF";
    short i;
    for (i = 7; i >= 0; i--) PStrAppendChar(dst, (unsigned char)digits[(v >> (i * 4)) & 0xF]);
}

static void LogHex(const char *text, unsigned long v)
{
    Str255 s; char c[300];
    PStrFromC(s, text); PStrAppendHex(s, v); PStrToC(s, c, sizeof(c)); GatewayLog(c);
}

static void LogText2(const char *text, const char *tail)
{
    Str255 s, t; char c[300];
    PStrFromC(s, text); PStrFromC(t, tail); PStrAppend(s, t); PStrToC(s, c, sizeof(c)); GatewayLog(c);
}

/* ========================================================================== */
/* Status block + Gestalt selector                                             */
/* ========================================================================== */

/*
 * Selector stub (Pascal calling convention: OSErr fn(OSType sel, long *resp)):
 *   MOVEA.L 4(SP),A0        ; response pointer
 *   MOVE.L  #block,(A0)     ; patched at install (offset 6)
 *   MOVEA.L (SP)+,A1        ; return address
 *   ADDQ.L  #8,SP           ; discard the two parameters
 *   CLR.W   (SP)            ; function result = noErr
 *   JMP     (A1)
 */
static const unsigned char kSelectorStub[18] = {
    0x20, 0x6F, 0x00, 0x04,
    0x20, 0xBC, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x5F,
    0x50, 0x8F,
    0x42, 0x57,
    0x4E, 0xD1
};

static void ResetDynamicFields(EmuStatus *s)
{
    s->heartbeat = TickCount();
    s->batUpdated = 0;
    s->briUpdated = 0;
    s->batPercent = -1;
    s->batState = kEmuBatUnknown;
    s->powerSource = kEmuSrcUnknown;
    s->runtimeMin = -1;
    s->wattsX10 = -1;
    s->brightness = -1;
    s->autoDim = -1;
    s->reqBrightness = -1;
    s->reqAutoDim = -1;
    s->gatewayVersion = kEmuGatewayVersion;
    s->serialState = kEmuSerialClosed;
    s->errorCount = 0;
    s->prefsDirty = 0;
    s->shutdownRequested = 0;
    s->pmScaledInfo = 0;             /* menu bar clock: X until data arrives */
    s->reqSleep = 0;
}

static Boolean SetupStatusBlock(void)
{
    long response = 0;
    EmuStatus *s;
    Ptr stub;
    unsigned long addr;
    OSErr err;

    /* Reuse a block published by an earlier gateway instance. */
    if (Gestalt((OSType)kEmuGestaltSelector, (void *)&response) == noErr && response != 0) {
        s = (EmuStatus *)response;
        if (EmuValidStatus(s)) {
            if (EmuGatewayAlive(s)) {
                GatewayLog("another Emu Serial Gateway is already running; quitting");
                gAlreadyRunning = true;
                return false;
            }
            gStatus = s;
            ResetDynamicFields(s);
            GatewayLog("reusing status block from a previous gateway instance");
            return true;
        }
        GatewayLog("fatal: Gestalt selector 'EmuG' is taken by something else");
        return false;      /* selector taken by something incompatible */
    }

    s = (EmuStatus *)NewPtrSysClear(sizeof(EmuStatus));
    if (s == 0L) return false;
    stub = NewPtrSys(sizeof(kSelectorStub));
    if (stub == 0L) { DisposePtr((Ptr)s); return false; }

    BlockMoveData((Ptr)kSelectorStub, stub, sizeof(kSelectorStub));
    addr = (unsigned long)s;
    stub[6] = (unsigned char)(addr >> 24);
    stub[7] = (unsigned char)(addr >> 16);
    stub[8] = (unsigned char)(addr >> 8);
    stub[9] = (unsigned char)(addr);
    if (TrapAvailable(0xA0BD)) FlushCodeCache();

    s->signature = kEmuBlockSignature;
    s->version = kEmuBlockVersion;
    s->size = (short)sizeof(EmuStatus);
    s->shortcutMods = 0;
    s->hotkeyEnabled = 0;
    ResetDynamicFields(s);

    err = EmuNewGestalt((OSType)kEmuGestaltSelector, (void *)stub);
    if (err != noErr) {
        LogNum("fatal: NewGestalt returned ", err);
        DisposePtr(stub);
        DisposePtr((Ptr)s);
        return false;
    }

    /* Verify the selector actually returns our block. */
    response = 0;
    err = Gestalt((OSType)kEmuGestaltSelector, (void *)&response);
    if (err != noErr || response != (long)s) {
        LogNum("fatal: selector verification failed, err ", err);
        LogHex("  response was $", (unsigned long)response);
        return false;
    }

    gStatus = s;
    return true;
}

/* ========================================================================== */
/* Preferences file                                                            */
/* ========================================================================== */

static OSErr OpenPrefsFile(short *refNum, Boolean create)
{
    short vRef = 0;
    long dirID = 0;
    FSSpec spec;
    OSErr err;
    Str255 name;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, true, &vRef, (void *)&dirID);
    if (err != noErr) return err;
    PStrFromC(name, "Emu Serial Gateway Prefs");
    err = FSMakeFSSpec(vRef, dirID, name, &spec);
    if (err == fnfErr) {
        if (!create) return err;
        err = FSpCreate(&spec, (OSType)kEmuGatewayCreator, (OSType)0x70726566UL /* 'pref' */, smSystemScript);
        if (err != noErr) return err;
    } else if (err != noErr) {
        return err;
    }
    return FSpOpenDF(&spec, fsRdWrPerm, refNum);
}

static void LoadPrefs(void)
{
    short ref = 0;
    GatewayPrefs p;
    long count = sizeof(GatewayPrefs);
    if (OpenPrefsFile(&ref, false) != noErr) return;
    if (FSRead(ref, (void *)&count, (Ptr)&p) == noErr && count == (long)sizeof(GatewayPrefs) &&
        p.magic == kPrefsMagic && p.version == kPrefsVersion) {
        gStatus->shortcutMods = p.shortcutMods;
        gStatus->hotkeyEnabled = p.hotkeyEnabled;
    }
    FSClose(ref);
}

static void SavePrefs(void)
{
    short ref = 0;
    GatewayPrefs p;
    long count = sizeof(GatewayPrefs);
    if (OpenPrefsFile(&ref, true) != noErr) return;
    p.magic = kPrefsMagic;
    p.version = kPrefsVersion;
    p.shortcutMods = gStatus->shortcutMods;
    p.hotkeyEnabled = gStatus->hotkeyEnabled;
    p.reserved = 0;
    if (SetFPos(ref, fsFromStart, 0) == noErr) {
        FSWrite(ref, (void *)&count, (Ptr)&p);
        SetEOF(ref, sizeof(GatewayPrefs));
    }
    FSClose(ref);
}

/* ========================================================================== */
/* Serial port — fully asynchronous                                            */
/* ========================================================================== */
/*
 * Under SheepShaver every serial read/write is a blocking host read()/write()
 * on a PTY, and the emulated Mac spins inside a synchronous PBRead/PBWrite
 * until it completes.  If the host side stops draining the PTY (bridge stopped
 * while socat still holds the pipe, buffers left full by an earlier session)
 * a synchronous write never returns and the whole machine stops.  So:
 *   - reads are issued only when the driver reports bytes waiting, and
 *   - every read and write is asynchronous; the event loop polls ioResult
 *     and gives up (pausing serial work) if a request does not complete.
 * The parameter blocks and buffers live in the system heap, so a request
 * still pending when the gateway quits can complete harmlessly.
 */

#define kAsyncReadSize      512
#define kAsyncWriteSize     (kLineMax + 2)
#define kWriteTimeoutTicks  (2L * 60L)
#define kReadTimeoutTicks   (5L * 60L)
#define kFlushMaxReads      16

typedef struct {
    ParamBlockRec pb;
    char          buf[kAsyncReadSize];
} AsyncReadIO;

typedef struct {
    ParamBlockRec pb;
    char          buf[kAsyncWriteSize];
} AsyncWriteIO;

static AsyncReadIO  *gReadIO = 0L;
static AsyncWriteIO *gWriteIO = 0L;
static Boolean gReadBusy = false, gWriteBusy = false;
static unsigned long gReadStarted = 0, gWriteStarted = 0;
static Boolean gSerialPaused = false;      /* a request timed out; waiting for it to drain */
static Boolean gOpenFailNotified = false;

/* Immediate control call: csParam <- param (len bytes, max 22). */
static OSErr SerControlNow(short refNum, short csCode, const void *param, short len)
{
    ParamBlockRec pb;
    short i;
    unsigned char *dst = (unsigned char *)pb.cntrlParam.csParam;
    const unsigned char *src = (const unsigned char *)param;
    pb.cntrlParam.ioCompletion = 0L;
    pb.cntrlParam.ioVRefNum = 0;
    pb.cntrlParam.ioCRefNum = refNum;
    pb.cntrlParam.csCode = csCode;
    for (i = 0; i < 22; i++) dst[i] = 0;
    if (param && len > 0) { if (len > 22) len = 22; for (i = 0; i < len; i++) dst[i] = src[i]; }
    return EmuPBControlImmed(&pb);
}

/* Immediate status call: number of bytes waiting in the input buffer. */
static OSErr SerInputCount(short refNum, long *count)
{
    ParamBlockRec pb;
    OSErr err;
    pb.cntrlParam.ioCompletion = 0L;
    pb.cntrlParam.ioVRefNum = 0;
    pb.cntrlParam.ioCRefNum = refNum;
    pb.cntrlParam.csCode = 2;                    /* kSERDInputCount */
    err = EmuPBStatusImmed(&pb);
    if (err == noErr) *count = *(long *)pb.cntrlParam.csParam;
    return err;
}

static void SerialFail(void)
{
    gSerialOK = false;
    gStatus->serialState = kEmuSerialError;
    gNextSerialRetry = TickCount() + kSerialRetryTicks;
    gTxState = txIdle;
    gTxKind = kindNone;
}

static Boolean SerialOpen(void)
{
    Str255 name;
    OSErr err;
    SerShk shk;
    short config = baud9600 + data8 + noParity + stop10;

    if (gReadIO == 0L || gWriteIO == 0L || gInBuf == 0L) { GatewayLog("serial: I/O blocks missing"); SerialFail(); return false; }

    GatewayLog("serial: OpenDriver .AOut ...");
    if (gOutRef == 0) {
        PStrFromC(name, ".AOut");
        err = OpenDriver(name, &gOutRef);
        if (err != noErr) { LogNum("serial: OpenDriver .AOut failed err ", err); gOutRef = 0; SerialFail(); return false; }
    }
    LogNum("serial: .AOut open, ref ", gOutRef);
    if (gInRef == 0) {
        PStrFromC(name, ".AIn");
        err = OpenDriver(name, &gInRef);
        if (err != noErr) { LogNum("serial: OpenDriver .AIn failed err ", err); gInRef = 0; SerialFail(); return false; }
    }
    LogNum("serial: .AIn open, ref ", gInRef);

    /* Configuration through immediate control calls; advisory on a PTY, so
       results are logged but never fatal. */
    err = SerControlNow(gOutRef, 8 /* kSERDConfiguration */, &config, sizeof(config));
    LogNum("serial: SerReset .AOut -> ", err);
    err = SerControlNow(gInRef, 8, &config, sizeof(config));
    LogNum("serial: SerReset .AIn -> ", err);
    {
        struct { Ptr p; short len; } bufParam;
        bufParam.p = gInBuf; bufParam.len = kInputBufferSize;
        err = SerControlNow(gInRef, 9 /* kSERDInputBuffer */, &bufParam, sizeof(bufParam));
        LogNum("serial: SerSetBuf -> ", err);
    }
    shk.fXOn = 0; shk.fCTS = 0; shk.xOn = 0x11; shk.xOff = 0x13;
    shk.errs = 0; shk.evts = 0; shk.fInX = 0; shk.null = 0;
    err = SerControlNow(gOutRef, 10 /* kSERDSerHShake */, &shk, sizeof(shk));
    LogNum("serial: SerHShake .AOut -> ", err);
    err = SerControlNow(gInRef, 10, &shk, sizeof(shk));
    LogNum("serial: SerHShake .AIn -> ", err);

    LogNum2("serial: open OK, refs out ", gOutRef, " in ", gInRef);
    gSerialOK = true;
    gSerialPaused = false;
    gStatus->serialState = kEmuSerialOpen;
    return true;
}

/* Bytes waiting in the driver's input buffer (synchronous status call; it
   never blocks: SheepShaver answers it with FIONREAD). -1 on error. */
static long SerialAvailable(void)
{
    long avail = 0;
    OSErr err;
    if (!gSerialOK) return -1;
    err = SerInputCount(gInRef, &avail);
    if (err != noErr) { LogNum("serial: input count err ", err); SerialFail(); return -1; }
    return avail;
}

/* Start an asynchronous write of len bytes (copied into the system-heap buffer). */
static Boolean SerialWriteStart(const char *text, long len)
{
    OSErr err;
    if (!gSerialOK || gWriteBusy || len <= 0 || len > kAsyncWriteSize) return false;
    BlockMoveData((Ptr)text, (Ptr)gWriteIO->buf, len);
    gWriteIO->pb.ioParam.ioCompletion = 0L;
    gWriteIO->pb.ioParam.ioResult = 1;
    gWriteIO->pb.ioParam.ioRefNum = gOutRef;
    gWriteIO->pb.ioParam.ioBuffer = (Ptr)gWriteIO->buf;
    gWriteIO->pb.ioParam.ioReqCount = len;
    gWriteIO->pb.ioParam.ioPosMode = 0;
    gWriteIO->pb.ioParam.ioPosOffset = 0;
    err = PBWriteAsync((ParmBlkPtr)&gWriteIO->pb);
    if (err != noErr && err != 1) { LogNum("serial: PBWriteAsync err ", err); SerialFail(); return false; }
    gWriteBusy = true;
    gWriteStarted = TickCount();
    return true;
}

/* Start an asynchronous read of up to max bytes (only called with bytes waiting). */
static Boolean SerialReadStart(long max)
{
    OSErr err;
    if (!gSerialOK || gReadBusy || max <= 0) return false;
    if (max > kAsyncReadSize) max = kAsyncReadSize;
    gReadIO->pb.ioParam.ioCompletion = 0L;
    gReadIO->pb.ioParam.ioResult = 1;
    gReadIO->pb.ioParam.ioRefNum = gInRef;
    gReadIO->pb.ioParam.ioBuffer = (Ptr)gReadIO->buf;
    gReadIO->pb.ioParam.ioReqCount = max;
    gReadIO->pb.ioParam.ioActCount = 0;
    gReadIO->pb.ioParam.ioPosMode = 0;
    gReadIO->pb.ioParam.ioPosOffset = 0;
    err = PBReadAsync((ParmBlkPtr)&gReadIO->pb);
    if (err != noErr && err != 1) { LogNum("serial: PBReadAsync err ", err); SerialFail(); return false; }
    gReadBusy = true;
    gReadStarted = TickCount();
    return true;
}

/* A request has not completed in time: the host side is not servicing the PTY.
   Stop issuing serial work; Service() resumes once the request drains. */
static void SerialStuck(const char *what)
{
    LogText2("serial: request did not complete: ", what);
    GatewayLog("serial: paused (is the bridge running and reading its PTY?)");
    gSerialPaused = true;
    gSerialOK = false;
    gStatus->serialState = kEmuSerialError;
    gNextSerialRetry = TickCount() + kSerialRetryTicks;
    gTxState = txIdle;
    gTxKind = kindNone;
}

/* Poll the outstanding write. Returns 1 done OK, 0 still busy, -1 failed. */
static short SerialWritePoll(void)
{
    short r;
    if (!gWriteBusy) return 1;
    r = gWriteIO->pb.ioParam.ioResult;
    if (r > 0) {
        if ((TickCount() - gWriteStarted) > (unsigned long)kWriteTimeoutTicks) { SerialStuck("write"); return -1; }
        return 0;
    }
    gWriteBusy = false;
    if (r < 0) { LogNum("serial: write completed with err ", r); SerialFail(); return -1; }
    return 1;
}

/* Poll the outstanding read. Returns bytes received (>=0, 0 = still busy or
   nothing), -1 failed; data is copied to dst when complete. */
static long SerialReadPoll(char *dst, long max)
{
    short r;
    long got;
    if (!gReadBusy) return 0;
    r = gReadIO->pb.ioParam.ioResult;
    if (r > 0) {
        if ((TickCount() - gReadStarted) > (unsigned long)kReadTimeoutTicks) { SerialStuck("read"); return -1; }
        return 0;
    }
    gReadBusy = false;
    if (r < 0) { LogNum("serial: read completed with err ", r); SerialFail(); return -1; }
    got = gReadIO->pb.ioParam.ioActCount;
    if (got < 0) got = 0;
    if (got > max) got = max;
    if (got > 0) BlockMoveData((Ptr)gReadIO->buf, (Ptr)dst, got);
    return got;
}

/* ========================================================================== */
/* Transaction state machine                                                   */
/* ========================================================================== */

static void NoteFailure(void);
static void NoteSuccess(void);
static void TxComplete(void);

static char gTxCmd[kAsyncWriteSize];
static long gTxCmdLen = 0;
static short gFlushReads = 0;

/* Begin a transaction.  Returns false if one is already in flight or the
   port is unusable (the caller may retry later). */
static Boolean TxStart(short kind, const char *cmd, long firstTicks, long quietTicks, long hardTicks)
{
    long len = CStrLen(cmd);
    if (gTxState != txIdle || !gSerialOK || gWriteBusy) return false;
    if (len <= 0 || len + 1 > kAsyncWriteSize) return false;
    BlockMoveData((Ptr)cmd, (Ptr)gTxCmd, len);
    gTxCmd[len] = '\r';
    gTxCmdLen = len + 1;

    gTxKind = kind;
    gRawLen = 0; gLineStart = 0; gLines = 0;
    gFirstLineSeen = false; gExpectMore = false;
    gReply[0] = 0; gReplyLen = 0;
    gFlushReads = 0;
    gTxFirstTicks = firstTicks; gTxQuietTicks = quietTicks; gTxHardTicks = hardTicks;
    gTxState = txFlush;
    return true;
}

static void TxAppendLine(const char *line, long len)
{
    if (gReplyLen + len + 1 >= kReplyMax) return;
    if (gReplyLen > 0) gReply[gReplyLen++] = '\r';
    BlockMoveData((Ptr)line, (Ptr)(gReply + gReplyLen), len);
    gReplyLen += len;
    gReply[gReplyLen] = 0;
}

/* Scan gRaw for newly completed lines; returns true when the reply is complete. */
static Boolean TxScanLines(void)
{
    long i;
    Boolean done = false;
    for (i = gLineStart; i < gRawLen; i++) {
        if (gRaw[i] == '\r' || gRaw[i] == '\n') {
            long len = i - gLineStart;
            gRaw[i] = 0;
            if (len > 0) {
                const char *line = gRaw + gLineStart;
                TxAppendLine(line, len);
                gLines++;
                if (!gFirstLineSeen) {
                    gFirstLineSeen = true;
                    gExpectMore = CStrStartsWith(line, "HA|") &&
                                  !CStrStartsWith(line, "HA|PAGES|") &&
                                  !CStrStartsWith(line, "HA|END");
                }
                if (CStrStartsWith(line, "HA|END")) done = true;
            }
            gLineStart = i + 1;
        }
    }
    if (gFirstLineSeen && !gExpectMore) done = true;
    if (gRawLen >= (long)sizeof(gRaw) - 1) done = true;          /* buffer full */
    return done;
}

/*
 * Service the in-flight transaction.  Never blocks.
 *
 * Completion rules (see emu-serial-bridge handlers):
 *   - a reply whose first line starts with "HA|" but is not "HA|PAGES|..."
 *     continues until an "HA|END" line;
 *   - every other reply is a single line;
 *   - as a fallback, quietTicks of silence after the first byte ends the
 *     reply, and hardTicks bounds the whole transaction.
 */
static void TxPump(void)
{
    unsigned long now = TickCount();
    long got, avail;

    if (gTxState == txIdle || !gSerialOK) return;

    switch (gTxState) {
        case txFlush:
            /* Discard whatever is waiting before we send. */
            if (gReadBusy) {
                got = SerialReadPoll(gRaw, 0);
                if (got < 0) return;
                if (gReadBusy) return;
            }
            avail = SerialAvailable();
            if (avail < 0) return;
            if (avail > 0 && gFlushReads < kFlushMaxReads) {
                gFlushReads++;
                SerialReadStart(avail);
                return;
            }
            gTxState = txSend;
            /* fall through */
        case txSend:
            if (!SerialWriteStart(gTxCmd, gTxCmdLen)) { if (!gSerialOK) NoteFailure(); return; }
            gTxState = txSending;
            return;

        case txSending: {
            short w = SerialWritePoll();
            if (w < 0) { NoteFailure(); return; }
            if (w == 0) return;
            gTxStart = now; gTxLastByte = now;
            gTxState = txWaiting;
            return;
        }

        case txWaiting: {
            Boolean done = false;
            if (gReadBusy) {
                got = SerialReadPoll(gRaw + gRawLen, (long)sizeof(gRaw) - 1 - gRawLen);
                if (got < 0) { NoteFailure(); return; }
                if (got > 0) { gRawLen += got; gTxLastByte = now; done = TxScanLines(); }
            } else {
                avail = SerialAvailable();
                if (avail < 0) { NoteFailure(); return; }
                if (avail > 0) {
                    long space = (long)sizeof(gRaw) - 1 - gRawLen;
                    if (space > 0) { SerialReadStart(avail < space ? avail : space); return; }
                    done = true;
                }
            }
            if (!done) {
                if (gFirstLineSeen && (now - gTxLastByte) > (unsigned long)gTxQuietTicks) done = true;
                else if (!gFirstLineSeen && (now - gTxStart) > (unsigned long)gTxFirstTicks) done = true;
                else if ((now - gTxStart) > (unsigned long)gTxHardTicks) done = true;
                else if (now < gTxStart) done = true;                 /* tick wrap: bail out */
            }
            if (done) {
                gTxState = txIdle;
                TxComplete();
            }
            return;
        }
        default:
            gTxState = txIdle;
            return;
    }
}

/* Pump until the transaction completes or maxTicks elapse.  Used only by the
   Apple Event relay, whose caller is blocked waiting for the reply; every
   step inside is non-blocking, so a dead bridge costs at most maxTicks. */
static Boolean TxWaitSync(long maxTicks)
{
    unsigned long start = TickCount();
    while (gTxState != txIdle) {
        TxPump();
        if (gTxState == txIdle) break;
        if ((TickCount() - start) > (unsigned long)maxTicks) {
            gTxState = txIdle;
            gTxKind = kindNone;
            return false;
        }
    }
    return true;
}

/* ========================================================================== */
/* Reply parsing                                                               */
/* ========================================================================== */

static Boolean TokenIs(const char *s, const char *tok, const char **rest)
{
    long n = CStrLen(tok);
    long i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != tok[i]) return false;
    }
    if (s[n] != 0 && s[n] != ' ' && s[n] != '\t') return false;
    if (rest) *rest = CSkipSpaces(s + n);
    return true;
}

static void NoteFailure(void)
{
    if (gStatus->errorCount < 32000) gStatus->errorCount++;
    if (gStatus->errorCount >= kMaxErrorsBeforeUnknown) {
        gStatus->batPercent = -1;
        gStatus->brightness = -1;
        gStatus->pmScaledInfo = 0;
    }
}

static void NoteSuccess(void)
{
    gStatus->errorCount = 0;
}

/* Parse an unsigned/negative decimal such as "9.1", "10", "-1" into tenths.
   Returns characters consumed (0 if no digits); negative values give -1. */
static short CParseDecimalX10(const char *s, long *outX10)
{
    short i = 0;
    long ip = 0, f1 = -1, f2 = -1;
    Boolean neg = false, digits = false;
    if (s[0] == '-') { neg = true; i = 1; }
    while (s[i] >= '0' && s[i] <= '9') { ip = ip * 10 + (s[i] - '0'); i++; digits = true; }
    if (s[i] == '.') {
        i++;
        if (s[i] >= '0' && s[i] <= '9') { f1 = s[i] - '0'; i++; digits = true; }
        if (s[i] >= '0' && s[i] <= '9') { f2 = s[i] - '0'; i++; }
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    if (!digits) return 0;
    if (neg) { *outX10 = -1; return i; }
    *outX10 = ip * 10 + (f1 < 0 ? 0 : f1) + (f2 >= 5 ? 1 : 0);
    return i;
}

/* BAT <pct> <CHG|DIS|UNK> [<min> [<watts> [<chg> [<shdn>]]]]
   emu-serial-bridge battery handler; CHG = external power present, DIS = on
   battery.  Older bridges send only the first two fields. */
static void ParseBatteryReply(const char *line)
{
    const char *p;
    long pct = -1, mins = -1, wattsX10 = -1, tmp;
    short src = kEmuSrcUnknown, state = kEmuBatUnknown;
    short chg = -1, shdn = 0;
    short n;

    if (!TokenIs(line, "BAT", &p)) { NoteFailure(); return; }
    n = CParseInt(p, &pct);
    if (n == 0) { NoteFailure(); return; }
    p = CSkipSpaces(p + n);

    if (TokenIs(p, "CHG", &p))      src = kEmuSrcAC;
    else if (TokenIs(p, "DIS", &p)) src = kEmuSrcBattery;
    else if (TokenIs(p, "UNK", &p)) src = kEmuSrcUnknown;
    else { NoteFailure(); return; }

    n = CParseInt(p, &tmp);
    if (n > 0) {
        mins = tmp; p = CSkipSpaces(p + n);
        n = CParseDecimalX10(p, &wattsX10);
        if (n > 0) {
            p = CSkipSpaces(p + n);
            n = CParseInt(p, &tmp);
            if (n > 0) {
                chg = tmp ? 1 : 0; p = CSkipSpaces(p + n);
                n = CParseInt(p, &tmp);
                if (n > 0) shdn = tmp ? 1 : 0;
            }
        }
    }

    if (src == kEmuSrcAC)
        state = (chg >= 0) ? (chg ? kEmuBatCharging : kEmuBatFull) : ((pct >= 100) ? kEmuBatFull : kEmuBatCharging);
    else if (src == kEmuSrcBattery)
        state = kEmuBatDischarging;

    if (pct < 0) pct = -1;
    if (pct > 100) pct = 100;
    gStatus->batPercent = (short)pct;
    gStatus->batState = state;
    gStatus->powerSource = src;
    gStatus->runtimeMin = (mins >= 0 && mins < 32000) ? (short)mins : -1;
    gStatus->wattsX10 = (wattsX10 >= 0 && wattsX10 < 32000) ? (short)wattsX10 : -1;
    gStatus->batUpdated = TickCount();

    /* Power Manager view of the same data (see EmuShared.h / powermgr.c) */
    {
        unsigned long packed = kEmuPMBatteryInstalled;
        long level = (pct >= 0) ? (pct * 255L) / 100L : 0;
        if (src == kEmuSrcAC) {
            packed |= kEmuPMChargerConnected;
            if (state == kEmuBatCharging) packed |= kEmuPMBatteryCharging;
        }
        gStatus->pmScaledInfo = packed | (unsigned long)(level & 0xFF);
    }

    if (shdn && !gStatus->shutdownRequested) {
        GatewayLog("battery: UPS requested shutdown (low battery)");
        Notify("The battery is nearly empty: the UPS has requested a shutdown. Connect the power adapter now.");
    }
    gStatus->shutdownRequested = shdn;
    NoteSuccess();
}

/* BRIGHT <pct>  or  OK BRIGHT <pct> */
static void ParseBrightnessReply(const char *line)
{
    const char *p = line;
    long v;
    if (TokenIs(p, "OK", &p)) { /* p advanced past OK */ }
    if (!TokenIs(p, "BRIGHT", &p)) { NoteFailure(); return; }
    if (CParseInt(p, &v) == 0) { NoteFailure(); return; }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    gStatus->brightness = (short)v;
    gStatus->briUpdated = TickCount();
    NoteSuccess();
}

/* AUTO <0|1>  or  OK AUTO <0|1> */
static void ParseAutoReply(const char *line)
{
    const char *p = line;
    long v;
    if (TokenIs(p, "OK", &p)) { }
    if (!TokenIs(p, "AUTO", &p)) { NoteFailure(); return; }
    if (CParseInt(p, &v) == 0) { NoteFailure(); return; }
    gStatus->autoDim = (v != 0) ? 1 : 0;
    NoteSuccess();
}

static const char *KindName(short kind)
{
    switch (kind) {
        case kindBat: return "BAT?"; case kindBri: return "BRI?"; case kindAuto: return "AUTO?";
        case kindSetBri: return "BRIGHT"; case kindSetAuto: return "AUTO"; case kindSleep: return "SLEEP"; case kindRelay: return "relay";
        default: return "?";
    }
}

/* Called when a transaction finishes (successfully or by timeout). */
static void TxComplete(void)
{
    short kind = gTxKind;
    Boolean early = (TickCount() - gLogStartTicks) < 120L * 60L;
    gTxKind = kindNone;
    if (gLines == 0 || early || gStatus->errorCount > 0 || kind == kindSetBri || kind == kindSetAuto || kind == kindSleep || kind == kindRelay) {
        Str255 s, t; char c[300];
        PStrFromC(s, "tx "); PStrFromC(t, KindName(kind)); PStrAppend(s, t);
        PStrFromC(t, gLines == 0 ? " -> TIMEOUT (no reply)" : " -> "); PStrAppend(s, t);
        if (gLines > 0) {
            short i;
            for (i = 0; i < 120 && gReply[i] != 0; i++) PStrAppendChar(s, (unsigned char)gReply[i]);
        }
        PStrToC(s, c, sizeof(c)); GatewayLog(c);
    }
    switch (kind) {
        case kindBat:
            if (gLines > 0) ParseBatteryReply(gReply); else NoteFailure();
            break;
        case kindBri:
        case kindSetBri:
            if (gLines > 0) ParseBrightnessReply(gReply); else NoteFailure();
            break;
        case kindAuto:
        case kindSetAuto:
            if (gLines > 0) ParseAutoReply(gReply); else NoteFailure();
            break;
        case kindSleep:
            break;      /* reply logged above; nothing to parse */
        case kindRelay:
        default:
            break;      /* the relay handler inspects gReply/gLines itself */
    }
}

/* ========================================================================== */
/* Polling and requests                                                        */
/* ========================================================================== */

static long PollInterval(long base)
{
    if (gStatus->errorCount >= kBackoffAfterErrors) return base * kBackoffMultiplier;
    return base;
}

static void BuildBrightCommand(char *cmd, short pct)
{
    Str255 s;
    PStrFromC(s, "BRIGHT ");
    PStrAppendNum(s, pct);
    PStrToC(s, cmd, 32);
}

/* True when `now` has reached `deadline`, robust against the tick counter
   wrapping and against a deadline far in the future after a clock jump. */
static Boolean Due(unsigned long now, unsigned long deadline, unsigned long window)
{
    if (now >= deadline) return true;
    if (deadline - now > window) return true;
    return false;
}

/* Locate the Finder's process serial number. */
static Boolean FindFinder(ProcessSerialNumber *out)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Str255 name;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        info.processInfoLength = sizeof(info);
        info.processName = name;
        info.processAppSpec = 0L;
        if (GetProcessInformation(&psn, &info) == noErr &&
            info.processSignature == (OSType)0x4D414353UL /* 'MACS' */ &&
            info.processType == (OSType)0x464E4452UL /* 'FNDR' */) {
            *out = psn;
            return true;
        }
    }
    return false;
}

/* Hand the foreground back to the Finder (called once, after the first
   event has been received so the launch switch has completed). */
static void SendToBack(void)
{
    ProcessSerialNumber finder;
    if (FindFinder(&finder)) {
        OSErr err = SetFrontProcess(&finder);
        if (err != noErr) LogNum("SetFrontProcess(Finder) err ", err);
        else GatewayLog("moved to the background");
    } else {
        GatewayLog("Finder not found; staying in front");
    }
}

static unsigned long gStartTicks = 0;
#define kOpenDelayAfterStart   (2L * 60L)     /* open 2 s after launch */

/* Launched from Startup Items, the Finder is already up; a short settle
   delay is all that is needed before opening the port. */
static Boolean ReadyToOpenSerial(void)
{
    return (TickCount() - gStartTicks) >= (unsigned long)kOpenDelayAfterStart;
}

static void Service(void)
{
    unsigned long now = TickCount();
    char cmd[32];

    gStatus->heartbeat = now;

    if (gStatus->prefsDirty) {
        gStatus->prefsDirty = 0;
        SavePrefs();
    }

    if (!gSerialOK) {
        if (gOutRef == 0 && !ReadyToOpenSerial()) return;   /* first open waits for the Finder */
        if (!Due(now, gNextSerialRetry, 2UL * kSerialRetryTicks)) return;
        gNextSerialRetry = now + kSerialRetryTicks;
        /* A stuck request must drain before the port is usable again. */
        if (gWriteBusy && gWriteIO->pb.ioParam.ioResult > 0) return;
        if (gReadBusy && gReadIO->pb.ioParam.ioResult > 0) return;
        gWriteBusy = false;
        gReadBusy = false;
        if (gSerialPaused) GatewayLog("serial: stuck request drained, resuming");
        if (!SerialOpen()) {
            if (!gOpenFailNotified) {
                gOpenFailNotified = true;
                Notify("Emu Serial Gateway: the modem port could not be opened. See Emu Serial Gateway Log in the Preferences folder.");
            }
            return;
        }
        gStatus->errorCount = 0;
        gNextBat = gNextBri = gNextAuto = now;    /* poll right away */
    }

    TxPump();
    if (gPaused) return;
    if (gTxState != txIdle) return;                /* one transaction at a time */

    /* Requests first, so the UI feels immediate */
    if (gStatus->reqBrightness >= 0) {
        short pct = gStatus->reqBrightness;
        if (pct > 100) pct = 100;
        gStatus->reqBrightness = -1;
        BuildBrightCommand(cmd, pct);
        gNextBri = now + kBriPollTicks;
        TxStart(kindSetBri, cmd, kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }
    if (gStatus->reqAutoDim >= 0) {
        short v = gStatus->reqAutoDim;
        gStatus->reqAutoDim = -1;
        TxStart(kindSetAuto, v ? "AUTO 1" : "AUTO 0", kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }
    if (gStatus->reqSleep != 0) {
        /* Finder's Sleep item or a click on the menu bar battery (via the
           Emu Power Manager's _Sleep).  The bridge may answer ERR if it
           has no SLEEP handler yet; that is logged and otherwise harmless. */
        LogNum("sleep requested, mode ", gStatus->reqSleep);
        gStatus->reqSleep = 0;
        TxStart(kindSleep, "SLEEP", kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }

    if (Due(now, gNextBat, 2UL * kBatPollTicks * kBackoffMultiplier)) {
        gNextBat = now + PollInterval(kBatPollTicks);
        TxStart(kindBat, "BAT?", kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }
    if (Due(now, gNextBri, 2UL * kBriPollTicks * kBackoffMultiplier)) {
        gNextBri = now + PollInterval(kBriPollTicks);
        TxStart(kindBri, "BRI?", kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }
    if (Due(now, gNextAuto, 2UL * kAutoPollTicks * kBackoffMultiplier)) {
        gNextAuto = now + PollInterval(kAutoPollTicks);
        TxStart(kindAuto, "AUTO?", kPollFirstByteTicks, kPollQuietTicks, kPollHardTicks);
        return;
    }
}

/* ========================================================================== */
/* Apple Events                                                                */
/* ========================================================================== */

static OSErr PutReplyText(AppleEvent *reply, const char *text)
{
    if (reply == 0L || reply->descriptorType == typeNull) return noErr;   /* no reply wanted */
    return AEPutParamPtr((AERecord *)reply, keyDirectObject, typeChar, text, CStrLen(text));
}

static void BuildStatusText(char *out, short max)
{
    Str255 s, t;
    PStrFromC(s, "GATEWAY ");
    PStrAppendNum(s, kEmuGatewayVersion);
    PStrFromC(t, " SERIAL "); PStrAppend(s, t); PStrAppendNum(s, gStatus->serialState);
    PStrFromC(t, " PAUSED "); PStrAppend(s, t); PStrAppendNum(s, gPaused ? 1 : 0);
    PStrFromC(t, " BAT ");    PStrAppend(s, t); PStrAppendNum(s, gStatus->batPercent);
    PStrFromC(t, " BRI ");    PStrAppend(s, t); PStrAppendNum(s, gStatus->brightness);
    PStrFromC(t, " ERRORS "); PStrAppend(s, t); PStrAppendNum(s, gStatus->errorCount);
    PStrToC(s, out, max);
}

static pascal OSErr HandleRelay(const AppleEvent *evt, AppleEvent *reply, long refcon)
{
    char cmd[kLineMax];
    char text[256];
    DescType actualType = 0;
    Size actualSize = 0;
    OSErr err;
    const char *rest;
    (void)refcon;

    err = AEGetParamPtr((AERecord *)evt, keyDirectObject, typeChar, &actualType,
                        (Ptr)cmd, (Size)(sizeof(cmd) - 1), &actualSize);
    if (err != noErr) return PutReplyText(reply, "ERR|GATEWAY|NOPARAM");
    if (actualSize < 0) actualSize = 0;
    if (actualSize > (Size)(sizeof(cmd) - 1)) actualSize = sizeof(cmd) - 1;
    cmd[actualSize] = 0;

    /* Trim trailing CR/LF/spaces */
    while (actualSize > 0 && (cmd[actualSize - 1] == '\r' || cmd[actualSize - 1] == '\n' ||
                              cmd[actualSize - 1] == ' '))
        cmd[--actualSize] = 0;
    if (actualSize == 0) return PutReplyText(reply, "ERR|GATEWAY|EMPTY");

    if (TokenIs(cmd, "GATEWAY", &rest)) {
        if (TokenIs(rest, "STATUS", 0L))  { BuildStatusText(text, sizeof(text)); return PutReplyText(reply, text); }
        if (TokenIs(rest, "PAUSE", 0L))   { gPaused = true;  return PutReplyText(reply, "OK PAUSED"); }
        if (TokenIs(rest, "RESUME", 0L))  { gPaused = false; return PutReplyText(reply, "OK RESUMED"); }
        if (TokenIs(rest, "VERSION", 0L)) {
            Str255 s;
            PStrFromC(s, "GATEWAY VERSION ");
            PStrAppendNum(s, kEmuGatewayVersion);
            PStrToC(s, text, sizeof(text));
            return PutReplyText(reply, text);
        }
        return PutReplyText(reply, "ERR|GATEWAY|BADCMD");
    }

    LogText2("ae relay: ", cmd);
    if (!gSerialOK) return PutReplyText(reply, "ERR|GATEWAY|SERIAL");

    /* Let any in-flight poll finish (bounded) before taking the port. */
    if (gTxState != txIdle) TxWaitSync(kPollHardTicks + 30);

    if (!TxStart(kindRelay, cmd, kRelayFirstByteTicks, kRelayQuietTicks, kRelayHardTicks))
        return PutReplyText(reply, "ERR|GATEWAY|SERIAL");
    if (!TxWaitSync(kRelayHardTicks + 60))
        return PutReplyText(reply, "ERR|GATEWAY|TIMEOUT");
    if (!gSerialOK) return PutReplyText(reply, "ERR|GATEWAY|SERIAL");
    if (gLines == 0) return PutReplyText(reply, "ERR|GATEWAY|TIMEOUT");

    /* A relayed command may have changed hardware state; refresh soon */
    gNextBri = 0;
    return PutReplyText(reply, gReply);
}

static pascal OSErr HandleOpenApp(const AppleEvent *evt, AppleEvent *reply, long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return noErr;
}

static pascal OSErr HandleQuit(const AppleEvent *evt, AppleEvent *reply, long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    gQuit = true;
    return noErr;
}

static Boolean InstallHandlers(void)
{
    OSErr err;
    err = AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(HandleOpenApp), 0, false);
    if (err != noErr) return false;
    err = AEInstallEventHandler(kCoreEventClass, (AEEventID)kAEReopenApplication, NewAEEventHandlerUPP(HandleOpenApp), 0, false);
    if (err != noErr) return false;
    err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(HandleQuit), 0, false);
    if (err != noErr) return false;
    err = AEInstallEventHandler((AEEventClass)0x6D697363UL /* 'misc' */, (AEEventID)0x6576616CUL /* 'eval' */,
                                NewAEEventHandlerUPP(HandleRelay), 0, false);
    if (err != noErr) return false;
    err = AEInstallEventHandler((AEEventClass)0x6D697363UL /* 'misc' */, (AEEventID)0x646F7363UL /* 'dosc' */,
                                NewAEEventHandlerUPP(HandleRelay), 0, false);
    if (err != noErr) return false;
    err = AEInstallEventHandler((AEEventClass)kEmuGestaltSelector /* 'EmuG' */, (AEEventID)0x7365726CUL /* 'serl' */,
                                NewAEEventHandlerUPP(HandleRelay), 0, false);
    if (err != noErr) return false;
    return true;
}

/* ========================================================================== */
/* Main                                                                        */
/* ========================================================================== */

int main(void)
{
    EventRecord event;
    long response = 0;

    MaxApplZone();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0L); InitCursor();
    {
        Str255 t;
        MenuHandle apple, file;
        t[0] = 1; t[1] = 0x14;                       /* Apple menu title */
        apple = NewMenu(1, t);
        if (apple) { AppendResMenu(apple, (ResType)0x44525652UL /* 'DRVR' */); InsertMenu(apple, 0); }
        PStrFromC(t, "File");
        file = NewMenu(128, t);
        if (file) { PStrFromC(t, "Quit/Q"); AppendMenu(file, t); InsertMenu(file, 0); }
        DrawMenuBar();
    }

    gLogStartTicks = TickCount();
    LogNum("gateway v", kEmuGatewayVersion);

    /* Apple Event Manager is required (System 7+). */
    if (Gestalt(gestaltAppleEventsAttr, (void *)&response) != noErr || (response & 1) == 0) {
        GatewayLog("fatal: Apple Event Manager not available");
        return 1;
    }

    if (!SetupStatusBlock()) {
        if (gAlreadyRunning) return 0;
        GatewayLog("fatal: could not publish status block (Gestalt)");
        Notify("Emu Serial Gateway could not start: unable to publish its status block.");
        return 2;
    }
    LogHex("status block at $", (unsigned long)gStatus);
    {
        long powr = 0;
        short code = gStatus->pmInstalled & 0xFF;
        static const char *codes[] = { "extension did not run", "installed", "skipped: machine already has a Power Manager",
                                       "skipped: _PowerMgrDispatch already implemented", "failed: no memory", "failed: Gestalt registration" };
        LogText2("Emu Power Manager: ", (code >= 0 && code <= 5) ? codes[code] : "unknown status");
        if (code == kEmuPMInstalled) {
            LogHex("  traps/flags installed $", (unsigned long)(gStatus->pmInstalled & 0xFF00));
            if (gStatus->pmInstalled & kEmuPMDidChain) GatewayLog("  chained in front of the existing _PowerMgrDispatch");
        }
        if (Gestalt((OSType)0x706F7772UL, (void *)&powr) == noErr) LogHex("  Gestalt 'powr' = $", (unsigned long)powr);
        else GatewayLog("  Gestalt 'powr': undefined");
        LogNum2("  traps implemented: A09E ", TrapAvailable(0xA09E) ? 1 : 0, " A08A ", TrapAvailable(0xA08A) ? 1 : 0);
        LogNum2("  traps implemented: A085 ", TrapAvailable(0xA085) ? 1 : 0, " A09F ", TrapAvailable(0xA09F) ? 1 : 0);
    }
    gReadIO = (AsyncReadIO *)NewPtrSysClear(sizeof(AsyncReadIO));
    gWriteIO = (AsyncWriteIO *)NewPtrSysClear(sizeof(AsyncWriteIO));
    gInBuf = NewPtrSys(kInputBufferSize);
    if (gReadIO == 0L || gWriteIO == 0L || gInBuf == 0L) { GatewayLog("fatal: no memory for serial I/O blocks"); return 5; }
    GatewayLog("serial I/O blocks allocated");
    LoadPrefs();
    GatewayLog("prefs loaded");
    if (!InstallHandlers()) {
        GatewayLog("fatal: AEInstallEventHandler failed");
        Notify("Emu Serial Gateway could not start: Apple Event handlers could not be installed.");
        gStatus->heartbeat = 0;
        return 3;
    }

    GatewayLog("Apple Event handlers installed");
    gStartTicks = TickCount();
    gNextBat = gNextBri = gNextAuto = gStartTicks;
    gNextSerialRetry = gStartTicks;
    GatewayLog("entering main loop");

    while (!gQuit) {
        long sleep = (gTxState != txIdle) ? kSleepBusyTicks : kSleepIdleTicks;
        Boolean gotEvent = WaitNextEvent(everyEvent, &event, sleep, 0L);
        if (!gSentToBack) { gSentToBack = true; SendToBack(); }
        if (gotEvent) {
            if (event.what == kHighLevelEvent)
                AEProcessAppleEvent(&event);
            else if (event.what == mouseDown) {
                WindowPtr w;
                short part = FindWindow(event.where, &w);
                if (part == inMenuBar) {
                    long sel = MenuSelect(event.where);
                    if (HiWord(sel) == 128 && LoWord(sel) == 1) gQuit = true;
                    HiliteMenu(0);
                } else if (part == inSysWindow) {
                    SystemClick(&event, w);
                }
            } else if (event.what == keyDown && (event.modifiers & cmdKey)) {
                long sel = MenuKey((CharParameter)(event.message & charCodeMask));
                if (HiWord(sel) == 128 && LoWord(sel) == 1) gQuit = true;
                HiliteMenu(0);
            }
        }
        Service();
    }

    /* Mark the block as no longer maintained; it stays allocated so the
       Gestalt selector never dangles. */
    GatewayLog(gWriteBusy || gReadBusy ? "quit (a serial request is still pending; its blocks stay allocated)" : "quit");
    gStatus->heartbeat = 0;
    gStatus->serialState = kEmuSerialClosed;
    gStatus->pmScaledInfo = 0;
    return 0;
}
