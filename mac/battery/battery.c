/*
 * battery.c — "Emu Battery" Control Strip module ('sdev' code resource).
 *
 * A pixel-faithful reimplementation of Apple's Battery Monitor module
 * (Control Strip 1.3.1, System 7.5.5) whose data source is the Emu Serial
 * Gateway status block instead of the PowerBook Power Manager.
 *
 * Tile layout (recovered from Apple's module):
 *
 *   [status icon 16px]
 *   [3px][etched divider 2px][3px][bar graph, 8 bars]          (Show Battery Level)
 *   [3px][divider][3px][32px dial, bottom aligned]             (Show Battery Consumption)
 *   [3px][divider][3px][H:MM, right justified, 5 digit widths] (Show Time Remaining)
 *   [popup arrow PICT 9x8, vertically centred]
 *
 * Consumption and time are hidden while running from external power, and
 * the bar graph blinks at 1 Hz when the level is unknown (gateway missing).
 *
 * Resources (all IDs identical to Apple's module so the artwork can be
 * reused verbatim):
 *   'ics#'/'ics4'/'ics8' 256 battery, 257 charging, 258 plugged/full,
 *                        259 plug only, 260 empty outline
 *   'ICN#'/'icl4'/'icl8' 270..276 consumption dial frames
 *   'PICT' 256           popup arrow
 *   'MENU' 256           three items, retitled at run time
 *   'STR#' 256           help + menu strings (see kStr* below)
 *
 * Build: Retro68, -Wl,--mac-flat -Wl,-eEMUBATTERYMODULE
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Memory.h>
#include <Resources.h>
#include <Menus.h>
#include <Icons.h>
#include <TextEdit.h>
#include <Fonts.h>
#include <OSUtils.h>
#include <Gestalt.h>
#include <TextUtils.h>
#include <Retro68Runtime.h>

#include "ControlStripGlue.h"
#include "EmuShared.h"
#include "MacUtil.h"

/* ---- Resource IDs -------------------------------------------------------- */
#define kIconStatusBase   256      /* ics# 256..260 */
#define kIconDialBase     270      /* ICN# 270..276 */
#define kPictArrow        256
#define kMenuID           256
#define kStringsID        256

enum {                             /* 'STR#' 256 indices */
    kStrHelpOnBattery = 1,
    kStrHelpCharging,
    kStrHelpACFull,
    kStrHelpNoGateway,
    kStrHelpUnknown,
    kStrShowLevel,
    kStrHideLevel,
    kStrShowConsumption,
    kStrHideConsumption,
    kStrShowTime,
    kStrHideTime,
    kStrPrefsName
};

enum { kIconOnBattery = 0, kIconCharging, kIconPlugged, kIconPlugOnly, kIconEmpty, kIconCount };
#define kDialFrames  7
#define kBarCount    8

enum { kMenuItemLevel = 1, kMenuItemConsumption = 2, kMenuItemTime = 3 };

/* Consumption dial scale: 3.0 W .. 12.0 W maps onto dial frames 0..6 */
#define kWattsMinX10   30
#define kWattsMaxX10   120

/* ---- Preferences (saved through the Control Strip) ----------------------- */
#define kPrefsVersion 1
typedef struct {
    short   version;
    Boolean showLevel;
    Boolean showConsumption;
    Boolean showTime;
    Boolean pad;
} BatteryPrefs;

/* ---- Module globals (locked handle, returned from sdevInitModule) -------- */
typedef struct {
    Handle      statusIcons[kIconCount];
    Handle      dialIcons[kDialFrames];
    PicHandle   arrowPict;
    MenuHandle  menu;
    Handle      strings;

    short       arrowWidth, arrowHeight;
    short       barWidth;           /* SBGetBarGraphWidth(kBarCount) */
    short       textWidth;          /* 5 * widest digit */
    short       textHeight;         /* ascent + descent + leading */
    Boolean     hasColor;
    Boolean     metricsValid;
    unsigned char timeSep;

    BatteryPrefs prefs;

    /* Availability (sticky once the gateway reports the field) */
    Boolean     consumptionAvail;
    Boolean     timeAvail;

    /* Currently displayed state */
    Boolean     gatewayAlive;
    Boolean     dataValid;
    Boolean     onAC;
    short       iconIndex;
    short       level;              /* bar index -1..kBarCount-1, or -2 unknown */
    short       dialIndex;
    short       runtimeMin;
    short       lastWidth;

    /* Blink state for unknown level */
    Boolean     blinkPhase;
    unsigned long nextBlinkTicks;
} BatteryGlobals;

typedef BatteryGlobals *BatteryGlobalsPtr;

/* ---- Forward declarations ------------------------------------------------ */
static long  DoInit(void);
static void  DoClose(BatteryGlobalsPtr g, Handle h);
static long  DoGetWidth(BatteryGlobalsPtr g, GrafPtr port);
static long  DoTickle(BatteryGlobalsPtr g, Rect *statusRect, GrafPtr port);
static void  DoDraw(BatteryGlobalsPtr g, Rect *statusRect, GrafPtr port);
static long  DoMouseClick(BatteryGlobalsPtr g, Rect *statusRect);
static long  DoSaveSettings(BatteryGlobalsPtr g);
static void  DoBalloonHelp(BatteryGlobalsPtr g, Rect *statusRect);
static void  RefreshState(BatteryGlobalsPtr g, Boolean *widthChanged, Boolean *contentChanged);
static void  ComputeTextMetrics(BatteryGlobalsPtr g, GrafPtr port);
static void  DrawSeparator(BatteryGlobalsPtr g, Rect *r);
static void  SetMenuTitles(BatteryGlobalsPtr g);

/* ========================================================================== */
/* Entry point                                                                */
/* ========================================================================== */

pascal long EmuBatteryModule(long message, long params, Rect *statusRect, GrafPtr statusPort)
{
    BatteryGlobalsPtr g = 0L;
    Handle gh = (Handle)params;
    SignedByte saveState = 0;
    long result = 0;

    RETRO68_RELOCATE();

    if (message != sdevInitModule) {
        if (gh == 0L) return 0;                  /* should never happen */
        saveState = HGetState(gh);               /* lock the globals only while we run, as Apple does */
        HLock(gh);
        g = *(BatteryGlobalsPtr *)gh;
        if (g == 0L) { HSetState(gh, saveState); return 0; }
    }

    switch (message) {
        case sdevInitModule:
            result = DoInit();
            break;
        case sdevCloseModule:
            DoClose(g, (Handle)params);
            result = 0;
            break;
        case sdevFeatures:
            result = sdevWantMouseClicks | sdevDontAutoTrack | sdevHasCustomHelp | sdevKeepModuleLocked;
            break;
        case sdevGetDisplayWidth:
            result = DoGetWidth(g, statusPort);
            break;
        case sdevPeriodicTickle:
            result = DoTickle(g, statusRect, statusPort);
            break;
        case sdevDrawStatus:
            DoDraw(g, statusRect, statusPort);
            break;
        case sdevMouseClick:
            result = DoMouseClick(g, statusRect);
            break;
        case sdevSaveSettings:
            result = DoSaveSettings(g);
            break;
        case sdevShowBalloonHelp:
            DoBalloonHelp(g, statusRect);
            break;
        default:
            break;
    }
    if (message != sdevInitModule && message != sdevCloseModule)
        HSetState(gh, saveState);
    return result;
}

/* ========================================================================== */
/* Initialisation / teardown                                                  */
/* ========================================================================== */

static void ReleaseAll(BatteryGlobalsPtr g)
{
    short i;
    for (i = 0; i < kIconCount; i++)
        if (g->statusIcons[i]) { DisposeIconSuite(g->statusIcons[i], true); g->statusIcons[i] = 0L; }
    for (i = 0; i < kDialFrames; i++)
        if (g->dialIcons[i]) { DisposeIconSuite(g->dialIcons[i], true); g->dialIcons[i] = 0L; }
    if (g->arrowPict) { DisposeHandle((Handle)g->arrowPict); g->arrowPict = 0L; }
    if (g->menu)      { DisposeMenu(g->menu); g->menu = 0L; }
    if (g->strings)   { DisposeHandle(g->strings); g->strings = 0L; }
}

static long DoInit(void)
{
    Handle h;
    BatteryGlobalsPtr g;
    long response;
    short i;
    OSErr err;
    unsigned long smallSel, largeSel;
    Handle prefsH = 0L;
    Str255 prefsName;

    h = NewHandleSysClear(sizeof(BatteryGlobals));
    if (h == 0L) return -1;
    HLock(h);
    g = *(BatteryGlobalsPtr *)h;

    /* Colour QuickDraw?  Determines which icon depths to detach. */
    g->hasColor = false;
    if (Gestalt(gestaltQuickdrawVersion, (void *)&response) == noErr && response >= gestalt8BitQD)
        g->hasColor = true;
    smallSel = g->hasColor ? (unsigned long)svAllSmallData : (unsigned long)svSmall1Bit;
    largeSel = g->hasColor ? (unsigned long)svAllLargeData : (unsigned long)svLarge1Bit;

    for (i = 0; i < kIconCount; i++) {
        err = SBGetDetachIconSuite(&g->statusIcons[i], (short)(kIconStatusBase + i), smallSel);
        if (err != noErr || g->statusIcons[i] == 0L) goto fail;
    }
    for (i = 0; i < kDialFrames; i++) {
        err = SBGetDetachIconSuite(&g->dialIcons[i], (short)(kIconDialBase + i), largeSel);
        if (err != noErr || g->dialIcons[i] == 0L) goto fail;
    }

    g->arrowPict = GetPicture(kPictArrow);
    if (g->arrowPict == 0L) goto fail;
    DetachResource((Handle)g->arrowPict);
    g->arrowHeight = (**g->arrowPict).picFrame.bottom - (**g->arrowPict).picFrame.top;
    g->arrowWidth  = (**g->arrowPict).picFrame.right  - (**g->arrowPict).picFrame.left;

    g->menu = GetMenu(kMenuID);
    if (g->menu == 0L) goto fail;
    DetachResource((Handle)g->menu);

    g->strings = Get1Resource('STR#', kStringsID);
    if (g->strings == 0L) goto fail;
    DetachResource(g->strings);

    g->barWidth = SBGetBarGraphWidth(kBarCount);
    if (g->barWidth <= 0) g->barWidth = 27;     /* defensive default */

    /* Time separator from the current international resource */
    g->timeSep = ':';
    {
        Handle itl0 = GetIntlResource(0);
        if (itl0 != 0L && *itl0 != 0L)
            g->timeSep = ((Intl0Ptr)*itl0)->timeSep;
        if (g->timeSep == 0) g->timeSep = ':';
    }

    /* Preferences: default = level shown, consumption/time hidden */
    g->prefs.version = kPrefsVersion;
    g->prefs.showLevel = true;
    g->prefs.showConsumption = false;
    g->prefs.showTime = false;
    SBGetDetachedIndString(prefsName, g->strings, kStrPrefsName);
    if (prefsName[0] > 0 && SBLoadPreferences(prefsName, &prefsH) == noErr && prefsH != 0L) {
        if (GetHandleSize(prefsH) >= (Size)sizeof(BatteryPrefs)) {
            BatteryPrefs *p = (BatteryPrefs *)*prefsH;
            if (p->version == kPrefsVersion) {
                g->prefs.showLevel = p->showLevel ? true : false;
                g->prefs.showConsumption = p->showConsumption ? true : false;
                g->prefs.showTime = p->showTime ? true : false;
            }
        }
        DisposeHandle(prefsH);
    }

    g->level = -2;
    g->iconIndex = kIconEmpty;
    g->runtimeMin = -1;
    g->nextBlinkTicks = TickCount() + 60;

    {
        Boolean w, c;
        RefreshState(g, &w, &c);
    }
    SetMenuTitles(g);
    HUnlock(h);                                  /* locked again on each call */
    return (long)h;

fail:
    ReleaseAll(g);
    DisposeHandle(h);
    return -1;
}

static void DoClose(BatteryGlobalsPtr g, Handle h)
{
    ReleaseAll(g);
    if (h) DisposeHandle(h);
}

/* ========================================================================== */
/* State from the gateway                                                     */
/* ========================================================================== */

static short LevelFromPercent(short pct)
{
    long raw, lvl;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    raw = ((long)pct * 255L) / 100L;                 /* 0..255 like the Power Manager */
    lvl = ((raw * (long)(kBarCount + 1)) >> 8) - 1;  /* Apple's scaling: -1..kBarCount-1 */
    if (lvl < -1) lvl = -1;
    if (lvl > kBarCount - 1) lvl = kBarCount - 1;
    return (short)lvl;
}

static short DialFromWatts(short wattsX10)
{
    long idx;
    if (wattsX10 < 0) return 0;
    if (wattsX10 <= kWattsMinX10) return 0;
    if (wattsX10 >= kWattsMaxX10) return kDialFrames - 1;
    idx = ((long)(wattsX10 - kWattsMinX10) * (kDialFrames - 1)) / (kWattsMaxX10 - kWattsMinX10);
    if (idx < 0) idx = 0;
    if (idx > kDialFrames - 1) idx = kDialFrames - 1;
    return (short)idx;
}

/* Reads the gateway block and updates the displayed-state fields.
   widthChanged: something that affects the tile width changed.
   contentChanged: something visible changed. */
static void RefreshState(BatteryGlobalsPtr g, Boolean *widthChanged, Boolean *contentChanged)
{
    EmuStatus *s = EmuFindStatus();
    Boolean alive = false, valid = false, onAC = false;
    short icon = kIconEmpty, level = -2, dial = 0, mins = -1;
    Boolean consAvail = g->consumptionAvail, timeAvail = g->timeAvail;

    if (s != 0L) {
        alive = EmuGatewayAlive(s);
        valid = EmuBatteryValid(s);
    }
    if (valid) {
        onAC = (s->powerSource == kEmuSrcAC);
        level = LevelFromPercent(s->batPercent);
        if (s->wattsX10 >= 0) { consAvail = true; dial = DialFromWatts(s->wattsX10); }
        if (s->runtimeMin >= 0) { timeAvail = true; mins = s->runtimeMin; }
        if (onAC) {
            icon = (s->batState == kEmuBatCharging) ? kIconCharging : kIconPlugged;
            if (s->batState == kEmuBatFull || s->batPercent >= 100) icon = kIconPlugged;
        } else {
            icon = kIconOnBattery;
        }
    } else {
        icon = kIconEmpty;
        level = -2;             /* unknown -> blink */
    }

    *widthChanged = (onAC != g->onAC) || (consAvail != g->consumptionAvail) ||
                    (timeAvail != g->timeAvail) || (valid != g->dataValid);
    *contentChanged = *widthChanged || (icon != g->iconIndex) || (level != g->level) ||
                      (dial != g->dialIndex) || (mins != g->runtimeMin) ||
                      (alive != g->gatewayAlive);

    g->gatewayAlive = alive;
    g->dataValid = valid;
    g->onAC = onAC;
    g->iconIndex = icon;
    g->level = level;
    g->dialIndex = dial;
    g->runtimeMin = mins;
    g->consumptionAvail = consAvail;
    g->timeAvail = timeAvail;
}

/* ========================================================================== */
/* Width                                                                      */
/* ========================================================================== */

static void ComputeTextMetrics(BatteryGlobalsPtr g, GrafPtr port)
{
    GrafPtr savePort;
    FontInfo fi;
    short c, w, widest = 0;

    if (port == 0L) return;
    GetPort(&savePort);
    SetPort(port);
    GetFontInfo(&fi);
    g->textHeight = fi.ascent + fi.descent + fi.leading;
    for (c = '0'; c <= '9'; c++) {
        w = CharWidth((CharParameter)c);
        if (w > widest) widest = w;
    }
    g->textWidth = widest * 5;
    g->metricsValid = true;
    SetPort(savePort);
}

static short CurrentWidth(BatteryGlobalsPtr g)
{
    short width = g->arrowWidth + 16;
    if (g->prefs.showLevel)
        width += g->barWidth + 8;
    if (g->prefs.showConsumption && g->consumptionAvail && !g->onAC)
        width += 32 + 8;
    if (g->prefs.showTime && g->timeAvail && !g->onAC)
        width += g->textWidth + 8;
    return width;
}

static long DoGetWidth(BatteryGlobalsPtr g, GrafPtr port)
{
    ComputeTextMetrics(g, port);                  /* Apple recomputes on every width query */
    if (g->textWidth <= 0) g->textWidth = 30;    /* defensive: 5 * 6 px */
    g->lastWidth = CurrentWidth(g);
    return g->lastWidth;
}

/* ========================================================================== */
/* Drawing                                                                    */
/* ========================================================================== */

/* Etched vertical divider: r->left += 3, gray line at left, white line at
   left+1 (colour) or a single 50% gray line (black & white).  The caller
   adds a further 3 pixels, matching Apple's layout. */
static void DrawSeparator(BatteryGlobalsPtr g, Rect *r)
{
    r->left += 3;
    if (g->hasColor) {
        RGBColor save, c;
        GetForeColor(&save);
        c.red = c.green = c.blue = 0x5555;
        RGBForeColor(&c);
        MoveTo(r->left, r->top);
        LineTo(r->left, r->bottom);
        c.red = c.green = c.blue = 0xFFFF;
        RGBForeColor(&c);
        MoveTo(r->left + 1, r->bottom);
        LineTo(r->left + 1, r->top);
        RGBForeColor(&save);
    } else {
        PenState ps;
        Pattern gray;
        short i;
        GetPenState(&ps);
        for (i = 0; i < 8; i++) gray.pat[i] = (i & 1) ? 0xAA : 0x55;
        PenPat(&gray);
        MoveTo(r->left, r->top);
        LineTo(r->left, r->bottom);
        SetPenState(&ps);
    }
}

static void DrawBatterySection(BatteryGlobalsPtr g, Rect *r)
{
    Rect iconRect = *r;
    Point pt;
    short level;

    iconRect.right = iconRect.left + 16;
    PlotIconSuite(&iconRect, atNone, ttNone, g->statusIcons[g->iconIndex]);
    r->left += 16;

    if (!g->prefs.showLevel) return;

    DrawSeparator(g, r);
    r->left += 3;

    level = g->level;
    if (level == -2) {                      /* unknown: blink 0 / -1 like Apple */
        level = g->blinkPhase ? 0 : -1;
    }
    pt.v = r->top;
    pt.h = r->left;
    SBDrawBarGraph(level, kBarCount, BarGraphFlatRight, pt);
    r->left += g->barWidth;
}

static void DrawConsumption(BatteryGlobalsPtr g, Rect *r)
{
    Rect dial;
    DrawSeparator(g, r);
    r->left += 3;
    dial = *r;
    dial.bottom -= 1;
    dial.top = dial.bottom - 32;
    dial.right = dial.left + 32;
    PlotIconSuite(&dial, atBottom, ttNone, g->dialIcons[g->dialIndex]);
    r->left += 32;
}

static void DrawTime(BatteryGlobalsPtr g, Rect *r)
{
    Rect box;
    Str255 text;
    short inset;

    DrawSeparator(g, r);
    r->left += 3;
    box = *r;
    box.right = box.left + g->textWidth;
    inset = ((box.bottom - box.top) - g->textHeight) / 2;
    if (inset > 0) InsetRect(&box, 0, inset);

    text[0] = 0;
    if (g->runtimeMin >= 0) {
        long hours = g->runtimeMin / 60;
        long mins  = g->runtimeMin - hours * 60;
        PStrAppendNum(text, hours);
        PStrAppendChar(text, g->timeSep);
        if (mins < 10) PStrAppendChar(text, '0');
        PStrAppendNum(text, mins);
    } else {
        PStrAppendChar(text, '-');
        PStrAppendChar(text, g->timeSep);
        PStrAppendChar(text, '-');
        PStrAppendChar(text, '-');
    }
    TETextBox((Ptr)&text[1], (long)text[0], &box, teFlushRight);
    r->left = box.right;
}

static void DoDraw(BatteryGlobalsPtr g, Rect *statusRect, GrafPtr port)
{
    GrafPtr savePort;
    Rect r;
    Rect arrow;
    RgnHandle saveClip;

    if (statusRect == 0L) return;
    GetPort(&savePort);
    if (port) SetPort(port);
    if (!g->metricsValid) ComputeTextMetrics(g, port ? port : savePort);

    saveClip = NewRgn();
    if (saveClip) { GetClip(saveClip); ClipRect(statusRect); }

    r = *statusRect;
    DrawBatterySection(g, &r);

    if (g->prefs.showConsumption && g->consumptionAvail && !g->onAC)
        DrawConsumption(g, &r);
    if (g->prefs.showTime && g->timeAvail && !g->onAC)
        DrawTime(g, &r);

    /* Popup arrow, vertically centred at the right end */
    arrow = r;
    arrow.right = arrow.left + g->arrowWidth;
    arrow.top = ((r.top + r.bottom) - g->arrowHeight) / 2;
    arrow.bottom = arrow.top + g->arrowHeight;
    DrawPicture(g->arrowPict, &arrow);

    if (saveClip) { SetClip(saveClip); DisposeRgn(saveClip); }
    SetPort(savePort);
}

/* ========================================================================== */
/* Periodic tickle                                                            */
/* ========================================================================== */

static long DoTickle(BatteryGlobalsPtr g, Rect *statusRect, GrafPtr port)
{
    Boolean widthChanged = false, contentChanged = false;
    unsigned long now = TickCount();
    Boolean blinkTick = false;
    short oldIcon = g->iconIndex;
    Boolean oldAlive = g->gatewayAlive, oldValid = g->dataValid, oldOnAC = g->onAC;
    long result = 0;

    RefreshState(g, &widthChanged, &contentChanged);

    /* Unknown level blinks once per second (only while not on AC) */
    if (g->prefs.showLevel && g->level == -2 && !g->onAC) {
        if (now >= g->nextBlinkTicks || now < g->nextBlinkTicks - 120) {
            g->nextBlinkTicks = now + 60;
            g->blinkPhase = !g->blinkPhase;
            blinkTick = true;
        }
    }

    if (!contentChanged && !blinkTick) return 0;

    if (widthChanged) {
        short newWidth = CurrentWidth(g);
        if (newWidth != g->lastWidth)
            return sdevResizeDisplay | sdevHelpStateChange;   /* strip re-lays out and redraws */
    }

    if (statusRect == 0L) return 0;

    /* Same width: repaint ourselves, exactly as Apple's module does inside
       its tickle.  When the status icon changed, erase its area first
       (the icons' masks differ, so stray pixels would remain). */
    if (oldIcon != g->iconIndex || oldAlive != g->gatewayAlive ||
        oldValid != g->dataValid || oldOnAC != g->onAC) {
        GrafPtr savePort;
        Rect iconRect = *statusRect;
        GetPort(&savePort);
        if (port) SetPort(port);
        iconRect.right = iconRect.left + 16;
        EraseRect(&iconRect);
        SetPort(savePort);
        result = sdevHelpStateChange;                          /* balloon text differs now */
    }
    DoDraw(g, statusRect, port);
    return result;
}

/* ========================================================================== */
/* Popup menu                                                                 */
/* ========================================================================== */

static void SetMenuTitles(BatteryGlobalsPtr g)
{
    Str255 s;
    if (g->menu == 0L || g->strings == 0L) return;
    SBGetDetachedIndString(s, g->strings, g->prefs.showLevel ? kStrHideLevel : kStrShowLevel);
    SetMenuItemText(g->menu, kMenuItemLevel, s);
    SBGetDetachedIndString(s, g->strings, g->prefs.showConsumption ? kStrHideConsumption : kStrShowConsumption);
    SetMenuItemText(g->menu, kMenuItemConsumption, s);
    SBGetDetachedIndString(s, g->strings, g->prefs.showTime ? kStrHideTime : kStrShowTime);
    SetMenuItemText(g->menu, kMenuItemTime, s);
}

static long DoMouseClick(BatteryGlobalsPtr g, Rect *statusRect)
{
    short item;
    long result = 0;

    if (g->menu == 0L || statusRect == 0L) return 0;

    SetMenuTitles(g);

    /* Item availability follows Apple's rules */
    if (g->dataValid) EnableItem(g->menu, kMenuItemLevel); else DisableItem(g->menu, kMenuItemLevel);
    if (g->consumptionAvail && !g->onAC) EnableItem(g->menu, kMenuItemConsumption); else DisableItem(g->menu, kMenuItemConsumption);
    if (g->timeAvail && !g->onAC) EnableItem(g->menu, kMenuItemTime); else DisableItem(g->menu, kMenuItemTime);

    item = SBTrackPopupMenu(statusRect, g->menu);
    switch (item) {
        case kMenuItemLevel:
            g->prefs.showLevel = !g->prefs.showLevel;
            result = sdevResizeDisplay | sdevNeedToSave | sdevHelpStateChange;
            break;
        case kMenuItemConsumption:
            g->prefs.showConsumption = !g->prefs.showConsumption;
            result = sdevResizeDisplay | sdevNeedToSave | sdevHelpStateChange;
            break;
        case kMenuItemTime:
            g->prefs.showTime = !g->prefs.showTime;
            result = sdevResizeDisplay | sdevNeedToSave | sdevHelpStateChange;
            break;
        default:
            break;
    }
    if (result) SetMenuTitles(g);
    return result;
}

/* ========================================================================== */
/* Preferences                                                                */
/* ========================================================================== */

static long DoSaveSettings(BatteryGlobalsPtr g)
{
    Handle h;
    Str255 prefsName;
    OSErr err;

    if (g->strings == 0L) return -1;
    SBGetDetachedIndString(prefsName, g->strings, kStrPrefsName);
    if (prefsName[0] == 0) return -1;

    h = NewHandleClear(sizeof(BatteryPrefs));
    if (h == 0L) return memFullErr;
    {
        BatteryPrefs *p = (BatteryPrefs *)*h;
        p->version = kPrefsVersion;
        p->showLevel = g->prefs.showLevel;
        p->showConsumption = g->prefs.showConsumption;
        p->showTime = g->prefs.showTime;
    }
    err = SBSavePreferences(prefsName, h);
    DisposeHandle(h);
    return (long)err;
}

/* ========================================================================== */
/* Balloon help                                                               */
/* ========================================================================== */

static void DoBalloonHelp(BatteryGlobalsPtr g, Rect *statusRect)
{
    Str255 s;
    short which;

    if (g->strings == 0L || statusRect == 0L) return;
    if (!g->gatewayAlive)       which = kStrHelpNoGateway;
    else if (!g->dataValid)     which = kStrHelpUnknown;
    else if (!g->onAC)          which = kStrHelpOnBattery;
    else if (g->iconIndex == kIconCharging) which = kStrHelpCharging;
    else                        which = kStrHelpACFull;
    SBGetDetachedIndString(s, g->strings, which);
    if (s[0] > 0) SBShowHelpString(statusRect, s);
}
