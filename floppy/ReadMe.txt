Emu Control Strip {VERSION}
{DATE}

Native Mac OS 7.5 battery and brightness controls for a Macintosh running
under SheepShaver or Basilisk II on a host computer with emu-serial-bridge
(for example a Raspberry Pi with a battery UPS). The battery tile is Apple's
Battery Monitor artwork and layout, the brightness tile follows the Sound
Volume module, the Brightness control panel is Apple's own panel with new
code behind the slider, and Apple's menu bar clock shows the battery. All of
it is driven by the host through the emu-serial-bridge serial link.

WHAT IS ON THIS DISK

  Extensions:Emu Power Manager
      A system extension that lets Apple's own menu bar clock show the
      battery (Date & Time > Clock Options > "Show the battery level"), by
      presenting the part of the PowerBook Power Manager that the clock
      reads. See "Power Manager" in Developer Notes for what it does.

  Startup Items:Emu Serial Gateway
      The application that owns the modem port. It polls the host for
      battery and brightness state, publishes it for the Control Strip
      modules and the control panel, and sends their requests back. It runs in the
      background and appears in the Application menu.

  Control Strip Modules:Emu Battery
      Battery level, charging state, and (on units that report them) time
      remaining and power consumption.

  Control Strip Modules:Emu Brightness
      Screen brightness in eight steps, Sound Volume style.

  Control Panels:Brightness
      Apple's Brightness control panel, now controlling the host's
      display backlight. Slider, digits 0 to 9, arrow keys, and the Shortcut dialog.

  Developer Notes
      How other applications can read the host's state or send commands
      through the gateway. See "Apple Events" and "Status Block".

REQUIREMENTS

  - The Control Strip (System 7.5.3 or later; present in 7.5.5).
  - emu-serial-bridge on the host with the battery protocol
    BAT <pct> <CHG|DIS|UNK> <min> <watts> <chg> <shdn>.
  - The emulator's modem port (seriala) mapped to the bridge's PTY.

INSTALLATION

  1. Drag "Emu Serial Gateway" from this disk's Startup Items folder into the
     Startup Items folder inside your System Folder.
  2. Drag "Emu Power Manager" from this disk's Extensions folder into
     the Extensions folder inside your System Folder.
  3. Drag "Emu Battery" and "Emu Brightness" into the Control Strip
     Modules folder inside your System Folder.
  4. Move Apple's original "Brightness" control panel from Control Panels
     into Control Panels (Disabled). It was written for the Macintosh
     Classic and refuses to open on this machine anyway. Then drag
     "Brightness" from this disk's Control Panels folder into Control Panels.
  5. Because the Power Manager extension makes the Mac look like a portable
     to Apple's software, move these PowerBook-only files out of the way:
     from Control Strip Modules, "Battery Monitor", "Sleep Now", "Power
     Settings" and "HD Spin Down" (into a folder of your own, for example
     "Control Strip (Disabled)"); from Control Panels, "PowerBook"
     and "PowerBook Setup" (into Control Panels (Disabled)).
  6. Choose Restart from the Special menu. Control Strip modules load at
     startup, and the gateway launches from Startup Items.
  7. Apple's battery appears next to the menu bar clock by itself: the
     clock's "Show the battery level" option is on by default and becomes
     available once a Power Manager is present. Date & Time > Clock Options
     is where to turn it off if you prefer.

  Make sure the bridge is running on the host before the emulator starts.
  If the bridge is restarted later, restart the emulator too: it keeps its
  old serial connection.

USING IT

  Battery tile
      The icon shows a plain battery while running on the battery, a battery
      with a lightning bolt while charging, and a battery with a plug when
      the adapter is connected and the battery is not charging. The bar
      graph shows the level in eight bars. Click the tile for a popup with
      Show/Hide Battery Level, Battery Consumption and Time Remaining.
      Consumption (a dial from about 3 W to 12 W) and Time Remaining
      (hours:minutes) are available when the host's battery monitor reports
      power draw and runtime (for example with the SunFounder PiPower 5)
      and, as on a PowerBook, only while running on the battery. Hosts
      whose UPS does not report them leave those items dimmed.

  Brightness tile
      Click for a popup of levels 7 (brightest) to 0 (dimmest). The current
      level is marked with a bullet.

  Brightness control panel
      Drag the slider between Off and Max, type a digit from 0 to 9, or use
      the Up and Down Arrow keys. The Shortcut button chooses the modifier
      keys for a keyboard shortcut; the system-wide shortcut itself is not
      yet implemented, so the keys work while the panel is open.

  Balloon Help
      Turn on Show Balloons in the Guide menu and point at the battery tile.
      The balloon tells you whether the gateway is running and what the
      battery is doing.

  Menu bar battery
      Apple's own clock battery: fills with the charge, shows the lightning
      bolt while charging, and an X when the gateway has no data. Clicking
      it calls the system's Sleep. On a Power Mac that already has one
      (Energy Saver) that is what runs; on a machine without one the
      extension provides Sleep itself and the gateway forwards it to the
      host as SLEEP over the bridge, which needs a bridge handler to do
      anything.

  Low battery
      When the UPS asks the host to shut down, the gateway shows an alert.
      Connect the adapter promptly; the host will power off shortly after.

IF SOMETHING DOES NOT WORK

  - The battery tile shows an empty outline with a blinking bar: no data is
    reaching the modules. Point at it with Balloon Help on. "The Emu Serial
    Gateway is not running" means the gateway did not start or has quit;
    "not available at the moment" means it is running but the host is not
    answering.
  - The gateway keeps a log: "Emu Serial Gateway Log" in the Preferences folder
    (open it with SimpleText), and a copy at /tmp/emu-serial-gateway.log on
    the host. Each serial step and every failed transaction is recorded.
  - "serial: paused" in the log means the host stopped reading the serial
    link. Restart emu-serial-bridge on the host first, then the emulator.
  - The gateway can be quit from the Application menu (File > Quit); it
    relaunches at the next restart.

UNINSTALL

  Remove the five files from the System Folder, move Apple's files back
  from wherever you parked them, and restart. The
  gateway's preferences file ("Emu Serial Gateway Prefs") and log in the
  Preferences folder can be thrown away.

CREDITS

  Code: (c) 2026 Scott Chamberlain, MIT licence. Source and the host-side
  components (emu-serial-bridge, battery-monitor, brightness-control) are
  at github.com/spc6486. The Control Strip module artwork,
  the Brightness control panel resources and the slider control definition
  are Apple's, from System 7.5.5, and remain Apple's property.
