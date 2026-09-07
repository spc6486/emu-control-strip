/*
 * brightness.c — "Emu Brightness" Control Strip module ('sdev').
 *
 * Follows Apple's Sound Volume module (Control Strip 1.3.1) exactly in
 * structure and interaction: the tile is a 16x16 icon followed by the 9x8
 * popup arrow; clicking pops up a menu of levels 7..0 with the current
 * level bulleted.  Selecting a level posts a brightness request to the
 * Emu Serial Gateway, which sends BRIGHT <pct> to the host.
 *
 * Resources:
 *   'ics#'/'ics4'/'ics8' 256   sun icon (derived from Apple's Brightness cdev art)
 *   'PICT' 256                 popup arrow
 *   'MENU' 256                 items "7" .. "0"
 *   'STR#' 256                 1: help (normal), 2: help (gateway missing)
 *
 * Build: Retro68, -Wl,--mac-flat -Wl,-eEMUBRIGHTNESSMODULE
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Memory.h>
#include <Resources.h>
#include <Menus.h>
#include <Icons.h>
#include <OSUtils.h>
#include <Gestalt.h>
#include <Retro68Runtime.h>

#include "ControlStripGlue.h"
#include "EmuShared.h"
#include "MacUtil.h"

#define kIconID     256
#define kPictArrow  256
#define kMenuID     256
#define kStringsID  256

enum { kStrHelpNormal = 1, kStrHelpNoGateway = 2 };

#define kBulletChar 0xA5          /* MacRoman bullet, as used by Sound Volume */

typedef struct {
    Handle      iconSuite;
    PicHandle   arrowPict;
    MenuHandle  menu;
    Handle      strings;
    short       arrowWidth, arrowHeight;
    Boolean     hasColor;
} BrightnessGlobals;

typedef BrightnessGlobals *BrightnessGlobalsPtr;

static long DoInit(void);
static void DoClose(BrightnessGlobalsPtr g, Handle h);
static void DoDraw(BrightnessGlobalsPtr g, Rect *statusRect, GrafPtr port);
static long DoMouseClick(BrightnessGlobalsPtr g, Rect *statusRect);
static void DoBalloonHelp(BrightnessGlobalsPtr g, Rect *statusRect);

/* ========================================================================== */

pascal long EmuBrightnessModule(long message, long params, Rect *statusRect, GrafPtr statusPort)
{
    BrightnessGlobalsPtr g = 0L;
    Handle gh = (Handle)params;
    SignedByte saveState = 0;
    long result = 0;

    RETRO68_RELOCATE();

    if (message != sdevInitModule) {
        if (gh == 0L) return 0;
        saveState = HGetState(gh);
        HLock(gh);
        g = *(BrightnessGlobalsPtr *)gh;
        if (g == 0L) { HSetState(gh, saveState); return 0; }
    }

    switch (message) {
        case sdevInitModule:
            result = DoInit();
            break;
        case sdevCloseModule:
            DoClose(g, (Handle)params);
            break;
        case sdevFeatures:
            result = sdevWantMouseClicks | sdevDontAutoTrack | sdevHasCustomHelp | sdevKeepModuleLocked;
            break;
        case sdevGetDisplayWidth:
            result = 16 + g->arrowWidth;
            break;
        case sdevPeriodicTickle:
            result = 0;                       /* nothing on the tile changes */
            break;
        case sdevDrawStatus:
            DoDraw(g, statusRect, statusPort);
            break;
        case sdevMouseClick:
            result = DoMouseClick(g, statusRect);
            break;
        case sdevSaveSettings:
            result = 0;
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

static void ReleaseAll(BrightnessGlobalsPtr g)
{
    if (g->iconSuite) { DisposeIconSuite(g->iconSuite, true); g->iconSuite = 0L; }
    if (g->arrowPict) { DisposeHandle((Handle)g->arrowPict); g->arrowPict = 0L; }
    if (g->menu)      { DisposeMenu(g->menu); g->menu = 0L; }
    if (g->strings)   { DisposeHandle(g->strings); g->strings = 0L; }
}

static long DoInit(void)
{
    Handle h;
    BrightnessGlobalsPtr g;
    long response;
    OSErr err;

    h = NewHandleSysClear(sizeof(BrightnessGlobals));
    if (h == 0L) return -1;
    HLock(h);
    g = *(BrightnessGlobalsPtr *)h;

    g->hasColor = false;
    if (Gestalt(gestaltQuickdrawVersion, (void *)&response) == noErr && response >= gestalt8BitQD)
        g->hasColor = true;

    err = SBGetDetachIconSuite(&g->iconSuite, kIconID,
                               g->hasColor ? (unsigned long)svAllSmallData : (unsigned long)svSmall1Bit);
    if (err != noErr || g->iconSuite == 0L) goto fail;

    g->arrowPict = GetPicture(kPictArrow);
    if (g->arrowPict == 0L) goto fail;
    DetachResource((Handle)g->arrowPict);
    g->arrowHeight = (**g->arrowPict).picFrame.bottom - (**g->arrowPict).picFrame.top;
    g->arrowWidth  = (**g->arrowPict).picFrame.right  - (**g->arrowPict).picFrame.left;

    g->menu = GetMenu(kMenuID);
    if (g->menu == 0L) goto fail;
    DetachResource((Handle)g->menu);
    if (CountMItems(g->menu) != kEmuLevelCount) goto fail;   /* resource sanity */

    g->strings = Get1Resource('STR#', kStringsID);
    if (g->strings == 0L) goto fail;
    DetachResource(g->strings);

    HUnlock(h);                                  /* locked again on each call */
    return (long)h;

fail:
    ReleaseAll(g);
    DisposeHandle(h);
    return -1;
}

static void DoClose(BrightnessGlobalsPtr g, Handle h)
{
    ReleaseAll(g);
    if (h) DisposeHandle(h);
}

/* ========================================================================== */

static void DoDraw(BrightnessGlobalsPtr g, Rect *statusRect, GrafPtr port)
{
    GrafPtr savePort;
    Rect r;

    if (statusRect == 0L) return;
    GetPort(&savePort);
    if (port) SetPort(port);

    r = *statusRect;
    r.right = r.left + 16;
    PlotIconSuite(&r, atNone, ttNone, g->iconSuite);

    r.left = r.right;
    r.right = r.left + g->arrowWidth;
    r.top = ((statusRect->top + statusRect->bottom) - g->arrowHeight) / 2;
    r.bottom = r.top + g->arrowHeight;
    DrawPicture(g->arrowPict, &r);

    SetPort(savePort);
}

/* ========================================================================== */

static long DoMouseClick(BrightnessGlobalsPtr g, Rect *statusRect)
{
    EmuStatus *s = EmuFindStatus();
    Boolean usable = (s != 0L) && EmuBrightnessValid(s);
    short level = -1, item, i;

    if (g->menu == 0L || statusRect == 0L) return 0;

    /* Menu item i (1..8) represents level 8-i, i.e. "7" at the top. */
    for (i = 1; i <= kEmuLevelCount; i++) {
        SetItemMark(g->menu, i, 0);
        if (usable) EnableItem(g->menu, i); else DisableItem(g->menu, i);
    }
    if (usable) {
        level = EmuPercentToLevel(s->brightness);
        SetItemMark(g->menu, (short)(kEmuLevelCount - level), (CharParameter)kBulletChar);
    }

    item = SBTrackPopupMenu(statusRect, g->menu);

    if (usable) SetItemMark(g->menu, (short)(kEmuLevelCount - level), 0);

    if (item >= 1 && item <= kEmuLevelCount && usable) {
        short newLevel = (short)(kEmuLevelCount - item);
        if (newLevel != level)
            EmuRequestBrightness(s, kEmuLevelPercent[newLevel]);
    }
    return 0;
}

/* ========================================================================== */

static void DoBalloonHelp(BrightnessGlobalsPtr g, Rect *statusRect)
{
    Str255 str;
    EmuStatus *s = EmuFindStatus();
    short which = (s != 0L && EmuGatewayAlive(s)) ? kStrHelpNormal : kStrHelpNoGateway;
    if (g->strings == 0L || statusRect == 0L) return;
    SBGetDetachedIndString(str, g->strings, which);
    if (str[0] > 0) SBShowHelpString(statusRect, str);
}
