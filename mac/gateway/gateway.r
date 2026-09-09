/*
 * gateway.r — SIZE resource for the Emu Serial Gateway application.
 * A normal application (launched from Startup Items) that runs in the
 * background: canBackground + acceptSuspendResume, high-level-event aware.
 */
#include "Processes.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    256 * 1024,
    160 * 1024
};
