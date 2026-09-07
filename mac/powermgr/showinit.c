#include <MacTypes.h>
#include <Quickdraw.h>
#include <Icons.h>
#include <OSUtils.h>
#include "showinit.h"

#define kShowInitVCheck   (*(unsigned short *)0x928)
#define kShowInitV        (*(short *)0x92A)
#define kShowInitH        (*(short *)0x92C)
#define kShowInitHCheck   (*(unsigned short *)0x92E)

typedef struct {
    QDGlobals qd;
    long      qdPtr;          /* A5 points here; holds the address of qd.thePort */
} FakeA5World;

static unsigned short ShowInitChecksum(short x)
{
    return (unsigned short)((((unsigned short)x << 1) | ((unsigned short)x >> 15)) ^ 0x1021);
}

void EmuShowInitIcon(short iconFamilyID)
{
    FakeA5World world;
    long oldA5;
    CGrafPort cport;
    GrafPort bport;
    Rect r;
    long response = 0;
    Boolean color;

    oldA5 = SetA5((long)&world.qdPtr);
    InitGraf(&world.qd.thePort);

    if (ShowInitChecksum(kShowInitH) != kShowInitHCheck) kShowInitH = 8;
    if (ShowInitChecksum(kShowInitV) != kShowInitVCheck) kShowInitV = (short)(world.qd.screenBits.bounds.bottom - 40);
    if (kShowInitH + 34 > world.qd.screenBits.bounds.right) { r.left = 8; r.top = (short)(kShowInitV - 40); }
    else { r.left = kShowInitH; r.top = kShowInitV; }
    r.right = (short)(r.left + 32);
    r.bottom = (short)(r.top + 32);

    color = (Gestalt(gestaltQuickdrawVersion, (void *)&response) == noErr && response >= gestalt8BitQD);
    if (color) {
        OpenCPort(&cport);
        PlotIconID(&r, atNone, ttNone, iconFamilyID);
        CloseCPort(&cport);
    } else {
        OpenPort(&bport);
        PlotIconID(&r, atNone, ttNone, iconFamilyID);
        ClosePort(&bport);
    }

    kShowInitH = (short)(r.left + 40);
    kShowInitV = r.top;
    kShowInitHCheck = ShowInitChecksum(kShowInitH);
    kShowInitVCheck = ShowInitChecksum(kShowInitV);
    SetA5(oldA5);
}
