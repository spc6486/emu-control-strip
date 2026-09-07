# Emu Serial Gateway: interfaces for other applications

The Emu Serial Gateway is the Mac-side endpoint of emu-serial-bridge. It owns
the modem port, polls the host, and exposes two interfaces that any Mac
application can use without touching the serial port itself.

## 1. Status block (read state, request brightness)

The gateway publishes a `EmuStatus` block in the system heap and
registers the Gestalt selector `'EmuG'` whose response is the block's
address. The layout is in `mac/common/EmuShared.h`; readers validate
`signature` (`'EmuG'`), `version` (1) and `size` before use.

```c
#include "EmuShared.h"
EmuStatus *s = EmuFindStatus();          /* NULL if no gateway has ever run */
if (s && EmuBatteryValid(s))                  /* gateway alive and data fresh */
    percent = s->batPercent;                 /* batState, powerSource, runtimeMin, wattsX10 ... */
if (s && EmuBrightnessValid(s))
    EmuRequestBrightness(s, 60);              /* gateway sends BRIGHT 60 within ~0.1 s */
```

Fields: `heartbeat` (TickCount of the gateway's last loop pass), `batPercent`
0..100 or -1, `batState` (unknown/discharging/charging/full), `powerSource`
(unknown/AC/battery), `runtimeMin` and `wattsX10` (-1 unavailable),
`brightness` 0..100 or -1, `autoDim`, `shutdownRequested`, `pmScaledInfo` (the
packed `GetScaledBatteryInfo` value served by the Emu Power Manager
extension), `pmInstalled`, and the request fields `reqBrightness`/`reqAutoDim`/
`reqSleep`. Mac OS 7 is cooperatively scheduled and
none of this runs at interrupt time, so no locking is needed. This is the
interface the Control Strip modules and the Brightness control panel use.

## 2. Apple Event relay (send any bridge command)

Any application that can send an Apple Event can send a bridge command
through the gateway and receive the reply text. Three event forms reach the
same handler:

| Client | Sends | Reply |
|---|---|---|
| HyperCard 2.1+ | `request "HA LIST" from program "Emu Serial Gateway"` | in `it` |
| AppleScript | `tell application "Emu Serial Gateway" to do script "BAT?"` | the result |
| Any Apple Event client | class `'EmuG'`, ID `'serl'`, direct parameter `typeChar` | direct parameter of the reply, `typeChar` |

Reply lines are separated by return characters. The gateway completes a
reply when it sees one line, or, for replies whose first line begins with
`HA|` (other than `HA|PAGES|`), when it sees `HA|END`; after that a quiet
period of a third of a second also ends it. The command is sent between the
gateway's own polls; polls never interleave with a relayed reply.

Meta commands answered by the gateway itself:

| Command | Reply |
|---|---|
| `GATEWAY STATUS` | one line: version, serial state, paused flag, last battery %, brightness, error count |
| `GATEWAY PAUSE` / `GATEWAY RESUME` | stop/resume the gateway's own polling (for code that must use the port directly) |
| `GATEWAY VERSION` | `GATEWAY VERSION 100` |

Errors from the gateway itself: `ERR|GATEWAY|SERIAL` (port not open),
`ERR|GATEWAY|TIMEOUT` (no reply within 6 s), `ERR|GATEWAY|EMPTY`,
`ERR|GATEWAY|NOPARAM`, `ERR|GATEWAY|BADCMD`. Bridge errors pass through
unchanged (`ERR ...`, `ERR|...`).

### HyperCard example

```hypertalk
request "HA ON 01" from program "Emu Serial Gateway"
put it into theReply                      -- "OK|01|Kitchen|light|ON|dimmer|"

request "HA LIST" from program "Emu Serial Gateway"
repeat with i = 1 to the number of lines of it
  if line i of it is "HA|END" then exit repeat
  -- HA|<id>|<name>|<domain>|<STATE>|<control>|<value>  or  HA|PAGE|<page>
end repeat
```

Stacks that keep the Chesley serial XCMDs can coexist by bracketing their
serial session with `request "GATEWAY PAUSE" ...` and `request "GATEWAY
RESUME" ...`. Note that the `request` mapping to the `'misc'/'eval'` event
is documented HyperCard behaviour but has not yet been exercised with
the gateway.

### Raw Apple Event (C, Retro68)

```c
AEAddressDesc target; AppleEvent evt, reply; ProcessSerialNumber psn;
/* find the gateway by creator 'EmGw' with GetNextProcess/GetProcessInformation */
AECreateDesc(typeProcessSerialNumber, &psn, sizeof(psn), &target);
AECreateAppleEvent('EmuG', 'serl', &target, kAutoGenerateReturnID, kAnyTransactionID, &evt);
AEPutParamPtr(&evt, keyDirectObject, typeChar, "BAT?", 4);
AESend(&evt, &reply, kAEWaitReply | kAECanInteract, kAENormalPriority, 600, NULL, NULL);
AEGetParamPtr(&reply, keyDirectObject, typeChar, &type, buf, sizeof buf, &size);
```
