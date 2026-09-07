/*
 * showinit.h — draw a startup icon in the INIT parade.
 *
 * Implements the shared "ShowINIT" convention: the next icon position is
 * kept in low memory at $92A/$92C with checksums at $928/$92E so
 * consecutive extensions line up.  The icon family (ICN#/icl4/icl8) is
 * plotted from the extension's own resource file.
 */
#ifndef EMU_SHOWINIT_H
#define EMU_SHOWINIT_H
#include <MacTypes.h>
void EmuShowInitIcon(short iconFamilyID);
#endif
