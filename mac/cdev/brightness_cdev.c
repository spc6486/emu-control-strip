/*
 * brightness_cdev.c — "Brightness" control panel ('cdev' code resource).
 *
 * Uses Apple's Brightness control panel resources (System 7.5.5: DITL,
 * slider CDEF 3 + 'sldr' + PICTs, icons, help strings) with new code that
 * reads and writes the backlight through the Emu Serial Gateway status block.
 *
 * Control panel DITL -4064 (local item numbers; the Control Panels host
 * prepends numItems of its own):
 *   1  userItem   slider area (no-op draw procedure installed)
 *   2  control    slider, CNTL -4048, CDEF 3, range 76..255 (Off .. Max)
 *   3  picture    divider
 *   4  control    "Shortcut" push button -> keyboard shortcut dialog
 *   5  statText   "Off"     6  statText "Max"     7  statText "Screen Brightness"
 *
 * Shortcut dialog DLOG/DITL -4047:
 *   1 Cancel  2 OK  3 Control  4 Shift  5 Option  6/7 explanatory text
 *   8 userItem (outline around the default button, item 1, as in Apple's DITL)
 *
 * Keyboard while the panel is open (per Apple's help text): 0..9 set the
 * brightness in tenths, Up/Down Arrow adjust by one step.
 *
 * Build: Retro68, -Wl,--mac-flat -Wl,-eEMUBRIGHTNESSCDEV
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Memory.h>
#include <Dialogs.h>
#if defined(__has_include)
#  if __has_include(<Controls.h>)
#    include <Controls.h>    /* Control Manager (Universal Interfaces); Multiversal folds it into Dialogs.h */
#  endif
#endif
#include <Events.h>
#include <Windows.h>
#include <OSUtils.h>
#include <Retro68Runtime.h>

#include "EmuShared.h"
#include "MacUtil.h"

/* ---- cdev messages and results (Inside Macintosh: More Toolbox) ---------- */
enum {
    initDev = 0, hitDev = 1, closeDev = 2, nulDev = 3, updateDev = 4,
    activDev = 5, deActivDev = 6, keyEvtDev = 7, macDev = 8, undoDev = 9,
    cutDev = 10, copyDev = 11, pasteDev = 12, clearDev = 13, cursorDev = 14
};
enum { cdevGenErr = -1, cdevMemErr = 0, cdevResErr = 1, cdevUnset = 3 };

/* ---- Items --------------------------------------------------------------- */
enum { iSliderArea = 1, iSlider = 2, iDivider = 3, iShortcutButton = 4, iOff = 5, iMax = 6, iTitle = 7 };
enum { dCancel = 1, dOK = 2, dControl = 3, dShift = 4, dOption = 5, dTextPress = 6, dTextChoose = 7, dOutline = 8 };

#define kShortcutDialogID   (-4047)
#define kSliderMin          76
#define kSliderMax          255
#define kPctMin             10
#define kPctMax             100
#define kPollTicks          30       /* refresh from the gateway twice a second */

#define kCharUpArrow        0x1E
#define kCharDownArrow      0x1F

/* ---- Storage ------------------------------------------------------------- */
typedef struct {
    short          version;
    short          numItems;         /* host item count, remembered at init */
    short          shownPct;         /* brightness reflected by the slider */
    Boolean        gatewayOK;
    Boolean        pad;
    unsigned long  lastPoll;
} CdevStorage;

typedef CdevStorage *CdevStoragePtr;

/* ---- Helpers ------------------------------------------------------------- */

static short PctFromSlider(short v)
{
    long pct;
    if (v < kSliderMin) v = kSliderMin;
    if (v > kSliderMax) v = kSliderMax;
    pct = kPctMin + ((long)(v - kSliderMin) * (kPctMax - kPctMin) + (kSliderMax - kSliderMin) / 2)
                    / (kSliderMax - kSliderMin);
    if (pct < kPctMin) pct = kPctMin;
    if (pct > kPctMax) pct = kPctMax;
    return (short)pct;
}

static short SliderFromPct(short pct)
{
    long v;
    if (pct < kPctMin) pct = kPctMin;
    if (pct > kPctMax) pct = kPctMax;
    v = kSliderMin + ((long)(pct - kPctMin) * (kSliderMax - kSliderMin) + (kPctMax - kPctMin) / 2)
                     / (kPctMax - kPctMin);
    if (v < kSliderMin) v = kSliderMin;
    if (v > kSliderMax) v = kSliderMax;
    return (short)v;
}

static ControlHandle GetSlider(DialogPtr dialog, short numItems)
{
    short type = 0;
    Handle h = 0L;
    Rect r;
    GetDialogItem(dialog, (short)(numItems + iSlider), &type, &h, &r);
    if (h == 0L) return 0L;
    if ((type & 0x7F) < 4 || (type & 0x7F) > 7) {
        /* Items 4..7 are control types (ctrlItem + button/check/radio/resCtrl). */
        return 0L;
    }
    return (ControlHandle)h;
}

static pascal void NullUserItem(DialogPtr dialog, short item)
{
    (void)dialog; (void)item;
}

static pascal void OutlineUserItem(DialogPtr dialog, short item)
{
    short type; Handle h; Rect r;
    PenState ps;
    (void)item;
    GetDialogItem(dialog, dCancel, &type, &h, &r);
    GetPenState(&ps);
    PenNormal();
    PenSize(3, 3);
    InsetRect(&r, -4, -4);
    FrameRoundRect(&r, 16, 16);
    SetPenState(&ps);
}

/* Push the slider's value to the hardware (via the gateway). */
static void ApplyPct(CdevStoragePtr st, DialogPtr dialog, short pct)
{
    EmuStatus *s = EmuFindStatus();
    ControlHandle slider = GetSlider(dialog, st->numItems);
    if (pct < kPctMin) pct = kPctMin;
    if (pct > kPctMax) pct = kPctMax;
    st->shownPct = pct;
    if (slider) SetControlValue(slider, SliderFromPct(pct));
    if (s != 0L && EmuGatewayAlive(s)) EmuRequestBrightness(s, pct);
}

/* Reflect gateway state in the dialog (slider position and enabled state). */
static void SyncFromGateway(CdevStoragePtr st, DialogPtr dialog, Boolean force)
{
    EmuStatus *s = EmuFindStatus();
    ControlHandle slider = GetSlider(dialog, st->numItems);
    Boolean ok = (s != 0L) && EmuBrightnessValid(s);

    if (slider == 0L) return;

    /* The slider stays active even without gateway data; it simply has
       nothing to send until the gateway is up. */
    st->gatewayOK = ok;
    if (ok) {
        short pct = s->brightness;
        if (pct != st->shownPct || force) {
            st->shownPct = pct;
            SetControlValue(slider, SliderFromPct(pct));
        }
    }
}

/* ---- Shortcut dialog ----------------------------------------------------- */

static void SetCheck(DialogPtr d, short item, Boolean on)
{
    short type; Handle h; Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    if (h) SetControlValue((ControlHandle)h, on ? 1 : 0);
}

static Boolean GetCheck(DialogPtr d, short item)
{
    short type; Handle h; Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    return (h != 0L) && (GetControlValue((ControlHandle)h) != 0);
}

static void RunShortcutDialog(void)
{
    DialogPtr d;
    GrafPtr savePort;
    EmuStatus *s = EmuFindStatus();
    short mods = (s != 0L) ? s->shortcutMods : 0;
    short type; Handle h; Rect r;
    short hit;
    unsigned char empty[1];

    d = GetNewDialog(kShortcutDialogID, 0L, (WindowPtr)-1L);
    if (d == 0L) { SysBeep(1); return; }

    GetPort(&savePort);
    SetPort((GrafPtr)d);

    /* Default-button outline (item 8 surrounds Cancel in Apple's DITL) */
    GetDialogItem(d, dOutline, &type, &h, &r);
    SetDialogItem(d, dOutline, type, (Handle)NewUserItemUPP(OutlineUserItem), &r);

    /* Two overlapping explanatory texts: show one, blank the other */
    empty[0] = 0;
    GetDialogItem(d, mods ? dTextChoose : dTextPress, &type, &h, &r);
    if (h) SetDialogItemText(h, empty);

    SetCheck(d, dControl, (mods & controlKey) != 0);
    SetCheck(d, dShift,   (mods & shiftKey) != 0);
    SetCheck(d, dOption,  (mods & optionKey) != 0);

    ShowWindow((WindowPtr)d);
    for (;;) {
        ModalDialog(0L, &hit);
        if (hit == dCancel) break;
        if (hit == dOK) {
            short newMods = 0;
            if (GetCheck(d, dControl)) newMods |= controlKey;
            if (GetCheck(d, dShift))   newMods |= shiftKey;
            if (GetCheck(d, dOption))  newMods |= optionKey;
            if (s != 0L) {
                s->shortcutMods = newMods;
                s->hotkeyEnabled = (newMods != 0) ? 1 : 0;
                s->prefsDirty = 1;
            }
            break;
        }
        if (hit == dControl || hit == dShift || hit == dOption)
            SetCheck(d, hit, !GetCheck(d, hit));
    }
    SetPort(savePort);
    DisposeDialog(d);
}

/* ========================================================================== */
/* Entry point                                                                */
/* ========================================================================== */

pascal long EmuBrightnessCdev(short message, short item, short numItems, short cdevPrivate,
                                  EventRecord *event, long cdevStorage, DialogPtr dialog)
{
    Handle storage = (Handle)cdevStorage;
    CdevStoragePtr st = 0L;
    (void)cdevPrivate;

    RETRO68_RELOCATE();

    if (message == macDev) return 1;              /* we run on this machine */

    if (message == initDev) {
        short type; Handle h; Rect r;
        storage = NewHandleClear(sizeof(CdevStorage));
        if (storage == 0L) return cdevMemErr;
        HLock(storage);
        st = *(CdevStoragePtr *)storage;
        st->version = 1;
        st->numItems = numItems;
        st->shownPct = -1;
        st->gatewayOK = false;
        st->lastPoll = 0;

        GetDialogItem(dialog, (short)(numItems + iSliderArea), &type, &h, &r);
        SetDialogItem(dialog, (short)(numItems + iSliderArea), type,
                      (Handle)NewUserItemUPP(NullUserItem), &r);

        if (GetSlider(dialog, numItems) == 0L) {
            DisposeHandle(storage);
            return cdevResErr;
        }
        SyncFromGateway(st, dialog, true);
        /* The CNTL resource defines the slider invisible; Apple's initDev shows
           it after setting its value (SetCtlValue, then ShowControl). */
        {
            ControlHandle slider = GetSlider(dialog, st->numItems);
            if (slider) ShowControl(slider);
        }
        return (long)storage;
    }

    if (storage == 0L || cdevStorage == cdevUnset) return cdevStorage;
    if (*storage == 0L) return cdevStorage;
    st = *(CdevStoragePtr *)storage;

    switch (message) {
        case closeDev:
            DisposeHandle(storage);
            return 0;

        case hitDev: {
            short local = item - numItems;
            if (local == iSlider) {
                ControlHandle slider = GetSlider(dialog, numItems);
                if (slider) ApplyPct(st, dialog, PctFromSlider(GetControlValue(slider)));
            } else if (local == iShortcutButton) {
                RunShortcutDialog();
            }
            break;
        }

        case nulDev: {
            unsigned long now = TickCount();
            if (now - st->lastPoll >= (unsigned long)kPollTicks || now < st->lastPoll) {
                st->lastPoll = now;
                SyncFromGateway(st, dialog, false);
            }
            break;
        }

        case keyEvtDev: {
            unsigned char c = (unsigned char)(event->message & charCodeMask);
            if (c >= '0' && c <= '9') {
                ApplyPct(st, dialog, (short)(kPctMin + (c - '0') * 10));
            } else if (c == kCharUpArrow) {
                ApplyPct(st, dialog, (short)(st->shownPct + 10));
            } else if (c == kCharDownArrow) {
                ApplyPct(st, dialog, (short)(st->shownPct - 10));
            }
            break;
        }

        case updateDev:
        case activDev:
        case deActivDev:
        case undoDev: case cutDev: case copyDev: case pasteDev: case clearDev:
        case cursorDev:
        default:
            break;
    }
    return cdevStorage;
}
