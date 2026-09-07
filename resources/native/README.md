# resources/native

Apple's resource forks from System 7.5.5, used verbatim for the artwork and
control-panel layout. They are Apple's property and are not distributed
here. Extract them from a System 7.5.5 disk image you own:

    python3 tools/extract_native.py /path/to/MacOS755.hda

which writes `Battery_Monitor.rsrc`, `Sound_Volume.rsrc` and
`Brightness.rsrc` into this directory after checking that every resource
the build needs is present. `build.sh` refuses to run without them.
