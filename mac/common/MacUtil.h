/*
 * MacUtil.h — small helpers shared by the Emu Control Strip sources.
 *
 * Everything here is static so it can be used from flat code resources
 * without any run-time library dependencies beyond the Toolbox.
 * No "\p" Pascal string literals are used anywhere in the project: strings
 * are built at run time with these helpers so behaviour does not depend on
 * compiler options.
 */

#ifndef EMU_MACUTIL_H
#define EMU_MACUTIL_H

#include <MacTypes.h>
#include <OSUtils.h>
#include <Traps.h>

#ifndef M68K_INLINE
#  if defined(__m68k__)
#    define M68K_INLINE(...) = { __VA_ARGS__ }
#  else
#    define M68K_INLINE(...)
#  endif
#endif

/* Constants missing from the Multiversal headers */
#ifndef controlKey
#  define controlKey 0x1000
#endif
#ifndef kOnSystemDisk
#  define kOnSystemDisk (-32768)
#endif
#ifndef typeLongInteger
#  define typeLongInteger 0x6C6F6E67UL   /* 'long' */
#endif
/* Universal Interfaces call it Secs2Date; Multiversal calls it SecondsToDate.
   Multiversal declares SecondsToDate with a #pragma parameter, so detect it
   through its trap enum, which only Multiversal defines. */
#if !defined(__MWERKS__) && !defined(SecondsToDate)
#  if defined(__has_include)
#    if !__has_include(<Multiverse.h>)
#      define SecondsToDate Secs2Date
#    endif
#  endif
#endif
#ifndef kAEReopenApplication
#  define kAEReopenApplication 0x72617070UL /* 'rapp' */
#endif

/* Copy a C string into a Pascal string (truncating at 255). */
static void PStrFromC(StringPtr dst, const char *src)
{
    short n = 0;
    while (src[n] != 0 && n < 255) { dst[n + 1] = (unsigned char)src[n]; n++; }
    dst[0] = (unsigned char)n;
}

/* Copy a Pascal string. */
static void PStrCopy(StringPtr dst, ConstStringPtr src)
{
    short n = src[0], i;
    for (i = 0; i <= n; i++) dst[i] = src[i];
}

/* Append a Pascal string to another (truncating at 255). */
static void PStrAppend(StringPtr dst, ConstStringPtr src)
{
    short n = dst[0], m = src[0], i;
    for (i = 1; i <= m && n < 255; i++) dst[++n] = src[i];
    dst[0] = (unsigned char)n;
}

/* Append a single character. */
static void PStrAppendChar(StringPtr dst, unsigned char c)
{
    if (dst[0] < 255) { dst[0]++; dst[dst[0]] = c; }
}

/* Append a decimal number. */
static void PStrAppendNum(StringPtr dst, long n)
{
    unsigned char tmp[16];
    short len = 0, i;
    unsigned long u;
    Boolean neg = (n < 0);
    u = neg ? (unsigned long)(-n) : (unsigned long)n;
    do { tmp[len++] = (unsigned char)('0' + (u % 10)); u /= 10; } while (u != 0 && len < 15);
    if (neg) PStrAppendChar(dst, '-');
    for (i = len - 1; i >= 0; i--) PStrAppendChar(dst, tmp[i]);
}

/* Convert a Pascal string to a C string in the caller's buffer. */
static void PStrToC(ConstStringPtr src, char *dst, short dstSize)
{
    short n = src[0], i;
    if (n > dstSize - 1) n = dstSize - 1;
    for (i = 0; i < n; i++) dst[i] = (char)src[i + 1];
    dst[n] = 0;
}

/* C string length (avoids pulling in the C library in code resources). */
static long CStrLen(const char *s)
{
    long n = 0;
    while (s[n] != 0) n++;
    return n;
}

/* Case-sensitive C string prefix test. */
static Boolean CStrStartsWith(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return false; }
    return true;
}

/* Parse a decimal integer (with optional leading '-') from a C string.
   Returns the number of characters consumed (0 if no digits). */
static short CParseInt(const char *s, long *out)
{
    short i = 0; long v = 0; Boolean neg = false;
    if (s[0] == '-') { neg = true; i = 1; }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    *out = neg ? -v : v;
    return i;
}

/* Skip spaces. */
static const char *CSkipSpaces(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* _NewGestalt (0xA3AD): D0 = selector, A0 = selector function, result in D0.
   Declared here because the Multiversal Interfaces (as of 2026) emit the
   plain _Gestalt trap word (0xA1AD) for NewGestalt, which performs a lookup
   instead of a registration and fails with gestaltUndefSelectorErr. */
#pragma parameter __D0 EmuNewGestalt(__D0, __A0)
pascal OSErr EmuNewGestalt(OSType selector, void *selectorFunction) M68K_INLINE(0xA3AD);

/* _ReplaceGestalt (0xA5AD): D0 = selector, A0 = new selector function.
   The previous function is returned in A0 and discarded here. */
#pragma parameter __D0 EmuReplaceGestalt(__D0, __A0)
pascal OSErr EmuReplaceGestalt(OSType selector, void *selectorFunction) M68K_INLINE(0xA5AD);

/* Immediate Device Manager calls (bit 9 set): the driver is entered directly
   and the request is never queued.  Used for serial configuration and the
   input-count status query, which SheepShaver's native serial driver answers
   inline; its queued path completes through DriverServicesLib instead. */
#pragma parameter __D0 EmuPBControlImmed(__A0)
pascal OSErr EmuPBControlImmed(ParmBlkPtr pb) M68K_INLINE(0xA204);
#pragma parameter __D0 EmuPBStatusImmed(__A0)
pascal OSErr EmuPBStatusImmed(ParmBlkPtr pb) M68K_INLINE(0xA205);

/* Is a trap implemented on this machine? */
pascal void FlushCodeCache(void) M68K_INLINE(0xA0BD);

static Boolean TrapAvailable(short trapWord)
{
    UniversalProcPtr unimpl = GetToolTrapAddress(0xA89F);
    UniversalProcPtr addr;
    if (trapWord & 0x0800)
        addr = GetToolTrapAddress(trapWord);
    else
        addr = GetOSTrapAddress(trapWord);
    return addr != unimpl;
}

#endif /* EMU_MACUTIL_H */
