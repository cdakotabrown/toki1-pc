Heartthrob Memorial PC Enhanced
===============================

An unofficial native PC port of "Tokimeki Memorial: Forever with You"
(PlayStation, 1995), built from a fan decompilation.  This is the program
only: it contains no disc data.  You need your own copy of the original
disc, dumped as a raw BIN/CUE.


1. SETUP
--------

Make a folder named disk next to the program and put your disc image in it:

  Heartthrob Memorial PC Enhanced.exe   (Linux: tokimeki-pc)
  README.txt
  disk\
    Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue
    Tokimeki Memorial - Forever with You (Japan) (Rev 1) (Track 1).bin
    Tokimeki Memorial - Forever with You (Japan) (Rev 1) (Track 2).bin

The supported disc is SLPS-00065, Rev 1:

  Track 1 .bin   702,104,928 bytes   SHA1 7b2f72d1319db27d0700070aa0bffe886b5ef7a4
  Track 2 .bin    45,151,344 bytes   SHA1 6cdd8500168ab4ecb18b78ef7589586d26bc4501

The file names do not matter as long as the .cue names the .bin files.
No PlayStation BIOS is needed.  Linux: Ubuntu 20.04, Debian 11 or newer;
start ./tokimeki-pc.

Start the program.  The first time, it checks your disc and extracts the
game data into data\ (about a minute; the disc is not needed after that).
You can also drag your .cue onto the window, or press Enter to choose it.
Missing Track 2?  The game never plays it: press C to continue without it.


2. THE FOLDER
-------------

Everything stays in this folder; to move the game, move the folder.

  disk\          your disc image (yours; only needed for the first run)
  data\          the game data made from your disc, the English text
                 (data\en\) and tokimeki-pc.log (what the last run did)
  saves\         memory card, quicksaves, extras -- back this folder up
  settings.ini   all options, with comments (the game also keeps the
                 window position in it)

screenshots\, textures\ and music\ appear when you first use them.  If the
folder is not writable (for example inside Program Files), the game uses
%APPDATA%\tokimeki-pc instead (Linux: ~/.config/tokimeki-pc and
~/.local/share/tokimeki-pc) and says so.  An empty file named
user-data.txt next to the program chooses that on purpose.


3. PLAYING
----------

The game plays in English (made from your own disc's files).  For the
original Japanese: settings menu (Ctrl+O), GAME, LANGUAGE.

Two profiles: "enhanced" (the default: fast loads, fast-forward with `,
backlog Ctrl+B, status Ctrl+I, rewind with Delete, sharper 3D, texture and
music packs) and "parity" (exactly like the PlayStation).  Switch in the
settings menu; saves work in both.

Keys (circle confirms, cross cancels, as on the Japanese PlayStation):

  Arrow keys            d-pad
  Enter, Space          circle (confirm)
  Backspace, Escape     cross (cancel)
  E / Q                 triangle / square
  A / D, Z / C          L1 / R1, L2 / R2
  F1 or S, Tab          START, SELECT
  Ctrl+H                show the controls
  Ctrl+O                settings menu (also: controller Guide button)
  F11 or Alt+Enter      fullscreen
  Print Screen, Ctrl+P  screenshot
  Ctrl+S                save browser
  F5 / F9               quicksave / quickload
  Ctrl+Q                quit

Controllers (Xbox / XInput, DualShock, Switch) work out of the box.  Every
key can be changed in settings.ini.

Texture and music packs: put a pack's folder in textures\packs\ or
music\packs\, then choose it in the settings menu.


4. TROUBLESHOOTING
------------------

The setup screen names the file, what was expected and what was found.

  "This is not the supported release"  another revision, or a patched or
      damaged image; only Rev 1 above works.
  "This is a cooked ISO"  dump the disc again as a raw BIN/CUE.
  "CHD / PBP / ECM images are not supported"  CHD: chdman extractcd -i
      game.chd -o game.cue.  ECM: unecm.  PBP cannot be converted back.
  "Not enough free disk space"  the extraction needs about 1.5 GB.
  "The game data is damaged or incomplete" (later)  press Enter: the
      damaged files go to data.damaged-1 and the data is extracted again.
  Anything else: read data\tokimeki-pc.log.  To check everything: settings
      menu, GAME DATA AND ABOUT, Verify game data (or run the program with
      --check).

Windows may ask before the first start (the program is not signed):
"More info", "Run anyway".  Keep the folder path short (for example
C:\Games\tokimeki-pc).  With OneDrive, right-click disk\ and choose "Always
keep on this device" before the first start.  If a controller does
nothing, close Steam (Steam Input can take it).


5. LICENCES
-----------

Licences: Settings > About / licences, or run with --licenses.

The port is free software under the GNU GPL version 3; its source code is
the archive published next to this download (the same name ending in
"-source.tar.gz").  Tokimeki Memorial is (c) and a trademark of Konami;
PlayStation is a trademark of Sony Interactive Entertainment.  This is an
unofficial fan project, not affiliated with or endorsed by Konami or Sony.
