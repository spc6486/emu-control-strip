/*
 * ControlStripGlue.h — Control Strip module interface for Retro68.
 *
 * Retro68's Multiversal Interfaces do not include Apple's ControlStrip.h,
 * so the message constants and the _ControlStripDispatch (0xAAF2) utility
 * calls are declared here.  The dispatch convention was verified against the
 * compiled code of Apple's own Battery Monitor and Sound Volume modules from
 * System 7.5.5: parameters are pushed Pascal-style, then
 *
 *      MOVE.W  #(paramWords<<8 | selector), D0
 *      _ControlStripDispatch
 *
 * and any result is left in the caller's Pascal result slot.  Retro68's
 * `pascal` calling convention reserves and reads that slot itself, so the
 * inline sequences below contain no trailing result pop.
 *
 * Module entry point (a 'sdev' code resource, ID 0):
 *
 *   pascal long Module(long message, long params, Rect *statusRect,
 *                      GrafPtr statusPort);
 *
 * If this header is compiled for a non-68k host (syntax checking only),
 * M68K_INLINE expands to nothing and the functions are simply undefined.
 */

#ifndef CONTROLSTRIP_GLUE_H
#define CONTROLSTRIP_GLUE_H

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Menus.h>
#include <Dialogs.h>

#ifndef M68K_INLINE
#  if defined(__m68k__)
#    define M68K_INLINE(...) = { __VA_ARGS__ }
#  else
#    define M68K_INLINE(...)
#  endif
#endif

/* ---- Messages sent to a module ------------------------------------------ */
enum {
    sdevInitModule      = 0,   /* initialize; return >= 0 (typically a handle) or < 0 to refuse */
    sdevCloseModule     = 1,   /* dispose everything */
    sdevFeatures        = 2,   /* return feature bits */
    sdevGetDisplayWidth = 3,   /* return width in pixels */
    sdevPeriodicTickle  = 4,   /* idle time; may draw; return result bits */
    sdevDrawStatus      = 5,   /* draw into statusRect */
    sdevMouseClick      = 6,   /* mouse down in statusRect; return result bits */
    sdevSaveSettings    = 7,   /* save prefs; return 0 on success (else called again) */
    sdevShowBalloonHelp = 8    /* show a help balloon */
};

/* ---- Feature bits returned from sdevFeatures ----------------------------- */
enum {
    sdevWantMouseClicks  = 1 << 0,
    sdevDontAutoTrack    = 1 << 1,
    sdevHasCustomHelp    = 1 << 2,
    sdevKeepModuleLocked = 1 << 3
};

/* ---- Result bits returned from tickle / mouse click ---------------------- */
enum {
    sdevResizeDisplay    = 1 << 0,
    sdevNeedToSave       = 1 << 1,
    sdevHelpStateChange  = 1 << 2,
    sdevCloseNow         = 1 << 3
};

/* ---- SBDrawBarGraph directions ------------------------------------------- */
enum {
    BarGraphSlopeLeft  = -1,
    BarGraphFlatRight  = 0,
    BarGraphSlopeRight = 1
};

/* ---- Utility routines provided by the Control Strip ---------------------- */

pascal Boolean SBIsControlStripVisible(void)
        M68K_INLINE(0x7000, 0xAAF2);

pascal void SBShowHideControlStrip(Boolean showIt)
        M68K_INLINE(0x303C, 0x0101, 0xAAF2);

pascal Boolean SBSafeToAccessStartupDisk(void)
        M68K_INLINE(0x7002, 0xAAF2);

pascal short SBOpenModuleResourceFile(OSType fileCreator)
        M68K_INLINE(0x303C, 0x0203, 0xAAF2);

pascal OSErr SBLoadPreferences(ConstStr255Param prefsResourceName, Handle *preferences)
        M68K_INLINE(0x303C, 0x0404, 0xAAF2);

pascal OSErr SBSavePreferences(ConstStr255Param prefsResourceName, Handle preferences)
        M68K_INLINE(0x303C, 0x0405, 0xAAF2);

pascal void SBGetDetachedIndString(StringPtr theString, Handle stringList, short whichString)
        M68K_INLINE(0x303C, 0x0506, 0xAAF2);

pascal OSErr SBGetDetachIconSuite(Handle *theIconSuite, short theResID, unsigned long selector)
        M68K_INLINE(0x303C, 0x0507, 0xAAF2);

pascal short SBTrackPopupMenu(const Rect *moduleRect, MenuHandle theMenu)
        M68K_INLINE(0x303C, 0x0408, 0xAAF2);

pascal short SBTrackSlider(const Rect *moduleRect, short ticksOnSlider, short initialValue)
        M68K_INLINE(0x303C, 0x0409, 0xAAF2);

pascal OSErr SBShowHelpString(const Rect *moduleRect, StringPtr helpString)
        M68K_INLINE(0x303C, 0x040A, 0xAAF2);

pascal short SBGetBarGraphWidth(short barCount)
        M68K_INLINE(0x303C, 0x010B, 0xAAF2);

pascal void SBDrawBarGraph(short level, short barCount, short direction, Point barGraphTopLeft)
        M68K_INLINE(0x303C, 0x050C, 0xAAF2);

pascal void SBModalDialogInContext(ModalFilterUPP filterProc, short *itemHit)
        M68K_INLINE(0x303C, 0x040D, 0xAAF2);

pascal OSErr SBGetControlStripFontID(short *fontID)
        M68K_INLINE(0x303C, 0x020E, 0xAAF2);

pascal OSErr SBGetControlStripFontSize(short *fontSize)
        M68K_INLINE(0x303C, 0x020F, 0xAAF2);

#endif /* CONTROLSTRIP_GLUE_H */
