# Player guide: Heartthrob Memorial PC Enhanced

How to install, run and configure Heartthrob Memorial PC Enhanced, the
unofficial native PC port of Tokimeki Memorial: Forever with You (SLPS-00065). This covers the settings file, keys,
controllers and troubleshooting. Building from source: `README.md` at the
top of the repository.

The port runs the game's own decompiled code. Everything described here sits
around the game: the window, scaling, key bindings and volumes. None of it
changes how the game plays.

## 1. Install and first run

You need two things: the game program (the download for your system) and
your own copy of the game, dumped as a raw BIN/CUE with both tracks. The
program contains no data from the disc (no art, music, video or script
text). No PlayStation BIOS, no Python and no other
tool is needed.

The supported disc is **Tokimeki Memorial: Forever with You (Japan) (Rev 1)**,
SLPS-00065:

| File | Bytes | SHA1 |
|---|---|---|
| Track 1 `.bin` | 702,104,928 | `7b2f72d1319db27d0700070aa0bffe886b5ef7a4` |
| Track 2 `.bin` | 45,151,344 | `6cdd8500168ab4ecb18b78ef7589586d26bc4501` |
| `PSX.EXE` on the disc | 690,176 | `2d8d474816e4bf937b09dc3b017f550f7ce4e14b` |

### Windows (10 or 11, 64-bit)

1. Unzip `tokimeki-pc-windows.zip` anywhere you can write to: your Desktop,
   Documents, `C:\Games`. Not `Program Files` (it still works there, but
   your data then goes to `%APPDATA%\tokimeki-pc`, see below). The folder
   holds `Heartthrob Memorial PC Enhanced.exe` and `README.txt`, nothing
   else. The exe is the whole game: SDL2, the licence texts (Settings >
   About / licences, or `--licenses`) and everything else are inside it,
   and it needs no DLL beside it.
2. Make a folder named `disk` next to the exe and put your disc
   image in it: the `.cue` and both `.bin` files.
   ```
   tokimeki-pc\
     Heartthrob Memorial PC Enhanced.exe
     README.txt
     disk\
       Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue
       Tokimeki Memorial - Forever with You (Japan) (Rev 1) (Track 1).bin
       Tokimeki Memorial - Forever with You (Japan) (Rev 1) (Track 2).bin
   ```
   The names may differ, as long as the `.cue` names the `.bin` files.
3. Double-click `Heartthrob Memorial PC Enhanced.exe`. The window opens on the **setup
   screen**: it finds your disc in `disk\`, checks it (the SHA1 of both
   tracks and of the game program), extracts the game data into `data\`
   and checks every extracted file (SHA-256), with a progress bar. It takes
   about a minute, once. Then the game starts in the same window.

   (Screenshot, described: the game's green message window with its
   copper-and-cream border over the game's mint-and-teal checked backdrop,
   "Heartthrob Memorial" in large pink letters at the top, "Preparing the
   game data" in yellow, the disc image and data paths, a pink progress bar
   with "Checking Track 1 (SHA1 of 702 MB)" and a percentage, and "Esc:
   Cancel" at the bottom. All of the port's own screens -- this one, the
   settings menu, the save browser, the status page, notes -- use the
   English patch's proportional pixel font and the game's window style.)
4. Later launches start the game directly (a quick size check of the data,
   about a second). The disc is not needed again; keep it to re-extract if
   the data is ever damaged.

Windows SmartScreen may ask once before starting the unsigned exe ("More
info", "Run anyway").

### Linux (x86-64)

1. Unpack `tokimeki-pc-linux-x86_64.tar.gz`: `tar xzf
   tokimeki-pc-linux-x86_64.tar.gz`. The `tokimeki-pc/` folder holds the
   `tokimeki-pc` program and `README.txt`. The program has SDL2
   built in and needs only the system C library, glibc 2.29 or newer:
   Ubuntu 20.04, Debian 11, Linux Mint 20, Fedora 30, RHEL 9, SteamOS 3 or
   anything newer (not Ubuntu 18.04, Debian 10 or RHEL 8), and a desktop session (X11 or
   Wayland; PulseAudio, PipeWire or ALSA for sound). Nothing to install.
   Keep the folder anywhere you can write to (your home folder): the game
   keeps its data and saves in it, as on Windows.
2. Make `tokimeki-pc/disk/` and put the `.cue` and both `.bin` files in it.
3. Run `./tokimeki-pc` (or double-click it in a file manager). The same
   setup screen appears, then the game.

### The setup screen

- **Where it looks for the disc**: first `disk/` beside the program (any
  `.cue`, else the Track 1 `.bin`); then the program's own folder, its
  parent folder, and Downloads, Documents and Desktop (one folder deep).
  Outside `disk/` only files whose size is the supported Track 1's (or a
  `.cue` naming one) are considered, and nothing is read until you agree:
  the screen says "Found your disc image ... It is not in the disk folder,
  but it can be used where it is" and Enter uses it.
- **No disc found**: the screen says what is missing ("There is no folder
  ...\disk yet", "The disk folder is empty", or what `disk\` holds instead)
  and lists the expected files and their SHA1s. Then either put the files
  there and press R, **drag the `.cue` onto the window**, or press **Enter
  to browse** (Windows: the standard Open dialog; Linux: zenity or kdialog
  when installed, else drag and drop).
- **Command line**: `tokimeki-pc --disc PATH` uses that image (a `.cue`, or
  the Track 1 `.bin` with its `(Track 2)` sibling or `--audio-track FILE`).
- **Errors** name the file, what was expected and what was found, and what
  to do; each also points to `data/tokimeki-pc.log`. The cases and their advice
  are in section 5. Track 2 is optional: it holds one CD audio track the
  game never plays, so when it is missing or differs the screen offers
  **C: continue without it**.
- **Escape** cancels a running setup (nothing is left behind) or quits.
  On "The game data is ready" (shown a moment before the game starts),
  Escape and the window's close button quit too: the data stays, and the
  next launch starts the game straight away.

### Where things live

The program keeps everything in **its own folder** (a portable install).
After the first run it looks like this:

```
Heartthrob Memorial PC Enhanced.exe   (Linux: tokimeki-pc)
README.txt
settings.ini
disk/
data/
saves/
```

| Path | What |
|---|---|
| `disk/` | your disc image (yours to add; only needed for setup) |
| `data/` | everything made from your disc: the extracted game data (about 1 GB, with `assets.manifest.json`), the English text in `data/en/` (section 2, Language), and `tokimeki-pc.log` / `tokimeki-pc.log.old` (what this run and the previous one did) |
| `saves/` | the memory card `card1.mcr`, `quicksave/` (quicksave slots, browser saves, autosaves, the resume state), `extras/`, `seed/`, `seen_text.dat` |
| `settings.ini` | the settings (section 2); its last section, `[state]`, is the game's own: the window's size and place and the one-time hints |
| `screenshots/` | Print Screen / Ctrl+P (made when first used) |
| `textures/dump/`, `textures/packs/` | texture dumps and packs (section 6; made when first used) |
| `music/dump/`, `music/packs/` | song dumps and music packs (section 3; made when first used) |

Back up `saves/` to keep your progress; move or copy the whole folder to
move the game. Nothing else is created beside the program: no lock or
temporary file stays behind after the setup or the English preparation (a
crashed run's lock never blocks the next start).

**An install from before 2026-11-07** had `assets/`, `assets-en/`,
`window.ini`, `hints.ini` and `tokimeki-pc.log` beside the program. The
first start of a newer version moves them over by itself, without
extracting anything again: `assets/` is renamed `data/`, `assets-en/`
becomes `data/en/`, the log moves into `data/`, and `window.ini` /
`hints.ini` are folded into `[state]` of `settings.ini` and deleted (your
own lines and comments in `settings.ini` stay as they are). If a folder
cannot be moved (another program has a file in it open), the old one keeps
being used and the next start tries again; the log says so (`[DATA]`
lines).

The memory card file is written in the background right after the game
saves to the card, through `card1.mcr.tmp` and a rename, so a slow disk or
a virus scanner looking at the new file never stalls the game, and an
interrupted write never leaves a damaged card. Quitting waits for it.

**When the folder is not writable** (for example inside `Program Files`),
or when a file named `user-data.txt` is next to the program, the per-user
folders are used instead and the setup screen says so:

| What | Windows | Linux |
|---|---|---|
| everything | `%APPDATA%\tokimeki-pc\` (the same `data/`, `saves/`, `settings.ini`) | `settings.ini` in `~/.config/tokimeki-pc/`; `data/`, `saves/` and the rest in `~/.local/share/tokimeki-pc/` |

`TOKI_CONFIG_DIR=DIR` puts everything in DIR. `TOKI_PORTABLE=1` / `0`, or
a file `portable.txt` next to the program, force the choice either way.
`[system] save_dir` in the settings (or `TOKI_SAVE_DIR`) moves just the
saves. The exact rules: `pc/include/pc_data_root.h`.

**Source checkouts and the Python launcher** keep their layout: a
`pc/build/tokimeki-pc` built from source is not portable by default
(settings in `~/.config/tokimeki-pc/`, data in `~/.local/share/tokimeki-pc/`),
and `./run_game.sh` / `pc/start_game.py` still prepare `pc/build/pc/assets/`
and pass it to the game, with the saves in `~/.local/share/tokimeki-pc/saves`. From a source checkout on
Linux (gcc/g++, make, Python 3.10+ and `libsdl2-dev`):

```sh
./run_game.sh --disc '/path/to/Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue'
./run_game.sh            # later launches
```

Its options (`--fullscreen`, `--windowed`, `--set SECTION.KEY=VALUE`,
`--settings FILE`, `--profile NAME`, `--where`, `--resume`,
`--prepare-only`) are described by `./run_game.sh --help`. The program
itself takes `--profile=parity|enhanced`, `--disc`, `--check` (verify the
disc and every game-data file and print a report, also written to the
log) and `--licenses`.

Quicksave slots, the save browser's saves, autosaves, continue-where-you-
left-off and rewind work the same on Windows and Linux (2026-11-02; before
that the Windows build reported "not available" for all of them). A PC
save loads only into the build of the game that wrote it, on the same
system: a Linux save does not load in the Windows game, nor the reverse.
If a build cannot take PC saves at all (for example an executable stripped
of its symbol table), its keys say `Save states are not available in this
build` and no autosave is attempted; the memory card works as usual.

The memory card is a standard 128 KB `.mcr` image. Any memory-card tool can
read it, and `pc/tools/mcr_tool.py` imports and exports single saves.

## 2. The settings file

The game writes `settings.ini` with every option, its default value and a
comment the first time it starts. Edit it while the game is closed. Delete it
to go back to the defaults; it is recreated on the next launch. Unknown keys
and bad values are reported on the console and ignored, so a typo never stops
the game from starting.

Order of precedence, from lowest to highest:

1. the defaults;
2. `settings.ini`, or the file named by `--settings` / `TOKI_SETTINGS`;
3. `--set` and the other launcher options, passed to the game as
   `TOKI_SET="section.key=value;..."`;
4. the older variables `TOKI_MOUSE`, `TOKI_MOUSE_SENS`, `TOKI_MOUSE_SWAP_GAP`,
   `TOKI_MOUSE_GRAB` and `TOKI_INPUT_DEVICE`.

Changes made with hotkeys while playing (fullscreen, filter, scale, scanlines,
vsync, volume) last until you quit. Changes made in the **settings menu**
(Ctrl+O, [section 3](#the-settings-menu)) are written back into
`settings.ini` when the menu closes: only the keys you changed, each in
place, so your comments, other values and unknown keys stay as they were.
The window's size, place and maximised state are saved to the `[state]`
section at the end of `settings.ini` when you quit (with
`remember_window = true`); the game writes only that section there, the
same careful way (your edits elsewhere in the file are never touched).

**Automated runs never read it.** If the game sees a test or tool
environment (`PC_TRACE`, `PC_TRACE_FRAMES`, `PC_INPUT`, `PC_TURBO`,
`PC_PARITY_DIR`, `PC_PARITY_FRAMES`, `PC_SHOT_DIR`, `PC_DBG_CMDS`,
`TOKI_INPUT_SCRIPT`, `AUTOPLAY`, `SCENE_NAV`, or `SDL_VIDEODRIVER=dummy` /
`offscreen`), or `TOKI_SETTINGS=off`, it uses the defaults and writes
nothing. The scene matrix, playthroughs and parity tools therefore behave the
same whatever you configured (`pc/tools/test_player_settings.py` checks
this).

### Reference

Top level:

| Key | Default | Meaning |
|---|---|---|
| `profile` | `enhanced` | Game profile. `enhanced` (the default) gives instant loads, a steady frame rate and the PC play features, handled by `pc/hal/profile.*`. `parity` is console-faithful timing. A settings file from an older version may still say `parity`; change it to switch. The settings module only stores the name; builds without the profile module ignore it. |

`[display]`

| Key | Default | Meaning |
|---|---|---|
| `fullscreen` | `false` | Start in desktop fullscreen. |
| `window_width`, `window_height` | `0` | First-launch window size; `0` picks the largest whole multiple of 320x240 that fits in 75% of the desktop (960x720 on 1080p, 1280x960 on 1440p). |
| `remember_window` | `true` | Reopen at the last size and place (from `[state]` at the end of `settings.ini`). This wins over `window_width` / `window_height` once a place is saved. |
| `scale` | `aspect` | `aspect`: 4:3, as large as fits, with black bars (letterbox or pillarbox). `integer`: the largest whole multiple of 320x240, the sharpest pixels but wider bars. `stretch`: fill the window, which distorts the picture. |
| `filter` | `nearest` | `nearest` gives crisp pixels; `linear` smooths them. |
| `scanlines` | `0` | Scanline strength, 0 to 100 percent (0 = off). It darkens the lower half of every game row, and only when the picture is at least 2x. It is drawn on top of the finished frame, so screenshots and the game's picture are unaffected. |
| `crt` | `off` | CRT filter (both profiles; presentation only): `off`, `lite` (soft scanlines), `aperture` (scanlines, an aperture-grille RGB stripe mask, a little glow and corner shading) or `shadow` (scanlines, a staggered slot mask, more glow and shading). Lost brightness is given back, so the picture is not darker overall. Needs the picture at 2x or more (a 640x480 window or larger); below that it is drawn plain. While a preset is on it replaces `filter` and `scanlines` (the picture is sampled smooth and has its own scanlines). Menus, the backlog and notices are drawn after it, sharp. Screenshots stay the plain game picture. `TOKI_CRT=<preset>` overrides it. Movies (FMV) are shown without it. |
| `crt_curvature` | `false` | With a CRT preset on: bend the picture like a tube (rounded edges, black outside). `TOKI_CRT_CURVATURE=1` / `0` overrides it. |
| `vsync` | `false` | Wait for the monitor refresh when presenting. This reduces tearing; frame pacing still follows the game's own clock. |

`[graphics]`

| Key | Default | Meaning |
|---|---|---|
| `resolution_scale` | `1` | Internal resolution for 3D, 1 to 4, **enhanced profile only**. 3D polygons (the TACO space shooter, polygon edges and textured polygons anywhere) are drawn at this many times 320x240: 2 = 640x480, 3 = 960x720, 4 = 1280x960. Pictures, sprites and text stay pixel for pixel as on the console. `1` is off. Parity ignores it. The extra detail shows only in a window (or screen) at least that big; screenshots taken with it on are saved at the higher resolution. 2 costs little; 4 needs a fast CPU. |
| `subpixel` | `true` | **Enhanced profile only**, with `resolution_scale` 2 or more. 3D models are placed at their exact, sub-pixel screen positions in the high-resolution picture, so moving 3D (TACO's ships and asteroids) glides smoothly instead of jittering on the console's pixel grid. The game itself is unaffected. `false` draws 3D on the console's grid, enlarged. |
| `texture_dump` | `false` | **Enhanced profile only.** Save every picture the game uses (portraits, backgrounds, menus) as a PNG in `textures/dump/SLPS-00065/`, for making a texture pack (section 6). It only reads; the game plays the same. |
| `texture_pack` | `""` | **Enhanced profile only.** A texture pack to draw instead of the original pictures: a folder name in `textures/packs/`, or a full path. `""` = none. A pack switches the internal resolution on (at the pack's own scale, 2x at least), even with `resolution_scale = 1`. Section 6. |

`[audio]`: volumes are percentages, applied in the PC mixer. At 100 you
hear the game exactly as it sets its own volumes.

| Key | Default | Meaning |
|---|---|---|
| `master` | `100` | Everything. |
| `music` | `100` | The SPU voices. |
| `sfx` | `100` | The SPU voices, multiplied with `music`; see the note below. |
| `voice` | `100` | Audio streamed from the disc: character voices, streamed music (`BGM378.XA`), the boot movie's sound and CD audio. |
| `music_pack` | `""` | **Enhanced profile only.** The music pack to play instead of the game's sequenced songs: the name of a folder in `music/packs/` (see "Music packs" in section 3). Empty = the original music. `TOKI_MUSIC_PACK` overrides it. |
| `music_dump` | `false` | Write every song the game plays, as the console's sound chip renders it, to `music/dump/<track id>.wav`: to find out which file is which song. Any profile; the sound you hear does not change. `TOKI_MUSIC_DUMP=1` does the same. |
| `soundfont` | `""` | **Enhanced profile only.** A SoundFont (`.sf2`) in `music/soundfonts/` (or a full path) that plays the game's sequenced music instead of the console's instruments (see "SoundFont mode" in section 3). Empty = the original. `TOKI_SOUNDFONT` overrides it. |
| `interpolation` | `gaussian` | **Enhanced profile only.** How the sound chip reads its samples between their points: `gaussian` is the console's (soft, a little muffled); `cubic` and `sinc` keep more of the treble (see "Sound quality"). `TOKI_SPU_INTERP` overrides it. |
| `reverb` | `original` | **Enhanced profile only.** `hq` computes the console's own reverb in floating point instead of 16-bit steps. `TOKI_SPU_REVERB` overrides it. |
| `output_rate` | `44100` | **Enhanced profile only, on the next launch.** `48000` runs the audio device at 48 kHz and converts the game's 44.1 kHz sound with a high-quality filter, for sound cards that run at 48 kHz. `TOKI_AUDIO_RATE` overrides it. |

> **Music and sound effects cannot be split yet** -- except while a music
> pack plays a song: then `music` is the pack's volume and `sfx` the SPU's.
> On the PlayStation, the
> sequenced music and the sound effects both play on the SPU's 24 voices, and
> the mixer cannot yet tell which voice is which. `music` and `sfx` are
> therefore multiplied onto one bus: lowering either lowers both. `voice` is
> a real split. The mixer separates the disc-audio input from the SPU voices
> using the SPU's own CD and main volumes. The game's lip-sync and level
> meters read the CD input before the volume is applied, so they are
> unaffected. Two small approximations: CD audio routed into the SPU reverb
> keeps its reverb tail, and a sum that clipped is re-weighted after the
> clip.

`[input]`

| Key | Default | Meaning |
|---|---|---|
| `mouse` | `true` | Offer a PS1 mouse on port 1 when you use the mouse. |
| `mouse_mode` | `absolute` | `absolute`: the game's hand cursor goes where your pointer is in the window, whatever the window size, scale mode or borders. `relative`: the hand moves by the mouse's motion and the OS cursor is hidden and confined, like a PS1 mouse on a desk. Use `absolute` under WSLg, remote desktop or a VM: there `relative` moves the hand the wrong way (see below). Environment: `TOKI_MOUSE_MODE`. |
| `mouse_sensitivity` | `1.0` | Relative mode only: host pixels per mouse count. The game's own cursor-speed option multiplies this. |
| `mouse_grab` | `true` | Hide the OS cursor while the game is in mouse mode: over the picture in absolute mode (it reappears over the black borders), hidden and confined in relative mode. |
| `mouse_swap_gap` | `1` | Empty-port polls when switching between pad and mouse. |
| `device` | `auto` | Port 1: `auto` (the last device used), `mouse`, `pad` or `none`. |
| `controller` | `true` | Use game controllers. |
| `controller_layout` | `positional` | `positional`: face buttons by position, as on a PS1 pad, so south = cross and east = circle. `confirm_south`: south confirms, east cancels. Explicit `[controller]` cross/circle lines win. |
| `stick_threshold` | `16000` | Left-stick deflection (of 32767) that counts as a d-pad press. It lets go under three quarters of this, so a stick resting near the edge does not flicker. |
| `trigger_threshold` | `8000` | Trigger travel (of 32767) that counts as L2/R2. |
| `port2_controller` | `false` | `true`: the second controller connected is a pad on PS1 port 2 (section 4). `false`: every controller is port 1. Environment: `TOKI_PORT2`. |
| `backlog_button` | `leftstick` | Controller button(s) that open and close the dialogue backlog (enhanced profile); `none` = keyboard and mouse wheel only. |
| `menu_button` | `guide, touchpad` | Controller button(s) that open the settings menu: the Guide / Xbox / PS / Home button, and the touchpad click of a DualShock 4 or DualSense. SDL GameController names, comma-separated; `none` = keyboard only. Where the system keeps the Guide button (Steam Deck, Windows' Xbox Game Bar), use the keyboard key or bind another button, for example `paddle1` (a back paddle) or `misc1` (Share / Capture). |

`[keyboard]`, `[controller]`: one line per PS1 button, `up down left
right circle cross square triangle l1 r1 l2 r2 start select`, each a
comma-separated list of names. `none` unbinds a button.

- Keyboard names are SDL key names: `Return`, `Space`, `Backspace`,
  `Escape`, `Tab`, `Left Shift`, `Keypad Enter`, `F1`, `A`, `1`, `Up` and so
  on (full list: <https://wiki.libsdl.org/SDL2/SDL_Scancode>; use the name
  without `SDL_SCANCODE_`, as SDL prints it).
- Controller names are SDL GameController names: `a b x y back start guide
  leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft
  dpright misc1 paddle1 paddle2 paddle3 paddle4 touchpad`, or an axis with
  a sign: `+leftx -leftx +lefty -lefty +rightx -rightx +righty -righty
  +lefttrigger +righttrigger`. `a` is always the bottom (south) face
  button, whatever the label says (a Nintendo pad's B, a PlayStation
  pad's cross); `b` the right one, `x` the left one, `y` the top one.

`[hotkeys]`: PC-side keys. The game never sees them. A key may carry
modifiers: `Ctrl+`, `Alt+`, `Shift+`, `GUI+` (the Windows or Command key).
The modifiers must match exactly, so `F11` and `Shift+F11` are different keys.

`[system]`

| Key | Default | Meaning |
|---|---|---|
| `pause_on_focus_loss` | `false` | Pause the game and its sound while another window has focus. |
| `toasts` | `false` | Show short notes in the corner: a hotkey changed something, a quicksave finished, a new gallery picture, ending or achievement. The first-run controls hint and error notes (a failed screenshot or save, "settings not saved") always show. |
| `screenshot_dir` | `""` | Screenshot folder; empty = `screenshots/` in the data directory. |
| `save_dir` | `""` | Folder for the memory card and quicksaves; empty = `saves/` in the data directory (through the launcher) or `saves/` in the working directory (the executable run directly). A relative path is relative to the game folder; `~/` is your home. `TOKI_SAVE_DIR` wins over it. |
| `screenshot_scale` | `2` | Screenshots are the game's 320x240 picture enlarged by this whole factor (1 to 8), as PNG. With `[graphics] resolution_scale` above 1 (enhanced) the screenshot is the high-resolution picture as shown, at 1:1, and this factor is not applied. |

`[saves]`: the PC saves (section 3, "Saves"). Both profiles. Automated runs
(section 2) never autosave or write a resume state.

| Key | Default | Meaning |
|---|---|---|
| `autosave` | `true` | Save automatically at each week's planning screen (the screen where you choose the week's activities, every Sunday night and on the first day), into `auto1`..`autoN`. |
| `autosave_every` | `month` | `month`: autosave at the first planning screen of each month (the week that crosses into it). `week`: at every week's planning screen. `TOKI_AUTOSAVE_EVERY` wins over it. |
| `autosave_slots` | `3` | How many autosaves to keep, 1 to 9; the oldest is overwritten. |
| `autosave_ending` | `true` | Also save when the ending (graduation day, the confession) starts, into the `ending` save, so you can watch it again. |
| `resume` | `true` | When you quit during a game, save where you are and offer to continue from there at the next launch. |

`[enhanced]`: play features of the `enhanced` profile. The `parity` profile ignores them.

| Key | Default | Meaning |
|---|---|---|
| `fast_forward_speed` | `4` | Game frames run per shown frame while the fast-forward key is held (0 to 64; 0 = as fast as the computer can). The `TOKI_FF_SPEED` environment / `toki.cfg` value wins over it. |
| `backlog` | `true` | Keep this session's dialogue pages for the backlog and allow voice replay (section 3, "Backlog and voice replay"). |
| `status` | `true` | The read-only status page, Ctrl+I (section 3, "Status at a glance"). |
| `status_hud` | `false` | A one-line note in the top right corner over the game: your stress and the girl whose bomb is highest. |
| `rewind` | `true` | Keep the last moments of play in memory so you can hold the rewind key (Delete) to go back (section 3, "Rewind"). Uses 100-200 MB of memory; `false` frees it. |
| `rewind_seconds` | `30` | How far back rewinding reaches, 5 to 300 seconds of play. |
| `rewind_interval_frames` | `10` | Game frames between two rewind points, 1 to 60 (60 = one second). Smaller steps back more finely and uses more memory. |
| `skip_intro` | `false` | Press START for you through the boot movie and the title animation, so the game starts at the title menu (section 3, "Skipping the intro and read dialogue"). Takes effect at the next launch. |
| `skip_seen` | `false` | While you hold the fast-forward key, also go past dialogue pages you have already read (in any session); it stops at new text. |
| `quick_text` | `true` | Confirm while a dialogue page is still being written shows the whole page at once; confirm again while its voice line plays cuts the voice and goes on (section 3, "Quick text"). `TOKI_QUICK_TEXT=0` / `1` overrides it. |

`[game]`: both profiles.

| Key | Default | Meaning |
|---|---|---|
| `language` | `en` | `en` plays the project's English translation (section 2, "Playing in English"); `ja` plays the original Japanese. Takes effect at the next launch. `TOKI_LANG=ja` / `en` overrides it, also in automated runs (which otherwise play in Japanese). A settings file written before 2026-11-05 says `ja`: change it to `en`, or delete the file, to switch. |

### Playing in English

English is the default for a new install (`language = en`; GAME > LANGUAGE
in the settings menu switches it, on restart). For the original Japanese set
`language = ja`. The English translation is built into the game; on the first English start
it is applied to **your own** extracted Japanese files and written to
`data/en/` (about 2 seconds; only the ~160 changed
files, about 13 MB). Later starts only check it. Japanese stays exactly as
it was, and you can switch back and forth with a restart. The memory card is
shared by both languages, but a name typed on the English (Latin) key page is
stored in Latin letters, which the original Japanese text engine was never
meant to draw: start an English game in English.

The English text is a draft, and a few pictures and menus stay Japanese.
If the console
prints `English unavailable; playing in Japanese`, your files are not the
supported disc (Rev 1) or the game was built without the translation pack.

## 3. Keys

The game confirms with **circle** and cancels with **cross**, the
Japanese PlayStation convention.

| Keyboard (default) | PS1 button |
|---|---|
| Arrow keys | d-pad |
| Enter, Space, keypad Enter | circle: confirm |
| Backspace, Escape | cross: cancel |
| E | triangle |
| Q | square |
| A / D | L1 / R1 |
| Z / C | L2 / R2 |
| F1, S | START |
| Tab | SELECT |

**Ctrl+H** shows this on screen, with your own bindings: which keyboard key
and which controller button each PS1 button is right now (section 3,
"Controls legend"). The first launch says so once.

A key pressed while Ctrl, Alt or GUI is held never presses a game button
(unless you bound a modifier key itself). That is why `Alt+Enter` toggles
fullscreen without also confirming a menu.

| Hotkey (default) | Action |
|---|---|
| F11, Alt+Enter | fullscreen / windowed |
| Print Screen, Ctrl+P | screenshot (PNG of the game picture) |
| Pause, Ctrl+Space | pause / resume. The game and its sound stop; the window stays live. |
| Ctrl+F | sharp / smooth filter |
| Ctrl+M | scale: aspect → integer → stretch |
| Ctrl+L | scanlines on / off (on = your `scanlines` value, or 50%) |
| Ctrl+V | vsync on / off |
| Ctrl+= / Ctrl+- | master volume up / down by 10% |
| Ctrl+Q | quit (closing the window also quits) |
| Ctrl+O | settings menu (below); the controller's Guide button does the same |
| F5 | quicksave into the selected slot (see below) |
| F9 | quickload the selected slot |
| ] / [ | select the next / previous quicksave slot and show the slot list |
| Ctrl+B, mouse wheel up | dialogue backlog (enhanced profile; see below) |
| Ctrl+R | replay the last voice line (enhanced profile) |
| Ctrl+S | the save browser (below): every save with its picture, date and stats |
| Ctrl+I | status at a glance (enhanced profile; see below): the girls, their bombs, your parameters, the calendar |
| Delete, Ctrl+Z (hold) | rewind (enhanced profile; see below) |
| Ctrl+H | controls legend: the key and controller button of every PS1 button (both profiles; see below) |

**Changed from earlier builds:** Escape no longer quits the game, because it
is a game key (cancel). Enter and Space are now circle (confirm); before,
they were cross, which cancels in this game. F11 is fullscreen; the
developer VRAM view moved to Shift+F11. F5 and F9 are the player's
quicksave keys (2026-11-01; they used to write a debug save state in the
working folder). The remaining F-keys (F2 to F4, F6 to F8, F10, F12) are
developer tools and harmless; F1 is
START. That is why the controls legend is Ctrl+H and not an F-key.

### Quicksave and quickload

**F5** saves the whole running game -- the exact moment, music and voice
included -- and **F9** puts it back, from anywhere: mid-conversation, in a
minigame, on a screen where the game itself offers no save. A note in the
corner confirms it (`Saved slot 3`, `Loaded slot 3`, `Slot 3 is empty`). It
works in both profiles, and it is a PC feature: the game never sees it.

**Slots.** There are nine slots, 1 to 9; F5 and F9 use the selected one
(slot 1 when the game starts). **]** and **[** select the next / previous
slot and show the slot list for three seconds:

```
Quicksave slot 3
 1  1995/04/12 Wed  11-01 21:04
 2  -
>3  1995/05/07 Sun  11-01 21:30
...
 0  1995/04/12 Wed  11-01 21:31 (undo)
```

Each line is the in-game date (or the game's screen number before a game is
under way), then when you saved it. **Slot 0** is automatic: every
quickload first saves the game as it was into slot 0, so an accidental F9
can be undone by selecting slot 0 and pressing F9. F5 never writes slot 0.

- Each slot is `quicksave/slotN.tks` (the state), `slotN.png` (a 160x120
  picture of the screen) and `slotN.ini` (date, stats, when, which build)
  in your saves folder (section 1). Deleting a slot's three files empties it.
- It loads only into the **same build** of the game (and the same system:
  a Linux quicksave does not load on Windows). After you update or
  rebuild the port, older quicksaves are refused (`Load slot 1 failed`);
  your memory card is unaffected. Use the game's own save (the memory card)
  for anything you want to keep across updates.
- A quickload never rolls back your **memory card**: albums you saved in the
  game after the quicksave stay on the card. The game's own screens show
  the card as the game last read it, so after a quickload, check the album
  list before overwriting an album.
- The key is acted on at the start of the next game frame. During the boot
  movie (one long frame) it waits until the movie ends; the note says
  `Saving slot 3...` / `Loading slot 3...` at once, whether the game was
  started through the launcher or directly, and the result replaces it.
  Pressed while the game is paused, or in the settings menu or the
  backlog, it is carried out when the game resumes; until then the pause
  label (or the menu's bottom line) says `Quicksave when you resume`.
- While the memory card is being written or read (a few frames, e.g.
  right after you save in the game), the quicksave or quickload waits
  for it to finish, so a quicksave never holds half a card write.
- A request that is not carried out says so: a second F5 to another
  slot before the first was taken (`Saving slot 4 instead of slot 3`), or
  a load that came first (`Save to slot 3 cancelled: a load came
  first`).
- Settings: `[hotkeys] quicksave`, `quickload`, `quick_slot_next`,
  `quick_slot_prev`; `none` disables one.
  Quicksave keys are inert in automated runs (section 2).

### Backlog and voice replay

**Enhanced profile only.** The port keeps every dialogue page of this
session -- the girls' lines, your own, the narration you waited on -- with
the voice line that went with it. Press **Ctrl+B**, turn the **mouse wheel
up**, or click the controller's **left stick** to scroll back through them;
the game pauses, with its sound, as for the settings menu.

| Key | In the backlog |
|---|---|
| Up / Down, the wheel (or your d-pad keys, the controller's d-pad) | the previous / next page; newest at the bottom |
| Page Up / Page Down (or left / right) | five pages |
| Home / End | the oldest / the newest page |
| Enter, Space, left click (or your circle keys / button) | hear the page's voice again (pages with a voice line carry a small speaker mark) |
| Escape, Backspace, right click, Ctrl+B, the wheel down past the newest page (or your cross keys / button, START) | close |

**Ctrl+R** replays the last voice line without opening anything. It plays
over the game at your **Voice** volume and stops when the game starts a
line of its own. The game itself never notices either: a replay is played
from the disc files by the PC, not through the game's sound program.

- The pages look exactly as they did in the game's message window: they
  are drawn with the game's own font, as it loaded it, in the game's
  colours. Only the message window is kept, not menus around it.
- A page you confirm through early (pressing circle while she is still
  speaking) is kept too; its replay is the whole line.
- The backlog lasts until you quit, and holds the latest 200 pages. It is
  not saved; a quickload keeps it (and what you read since).
- The settings menu (Ctrl+O) has the same things under **Dialogue**:
  `Backlog` turns it off (`[enhanced] backlog = false`), `Show backlog`,
  `Replay last voice`.
- Settings: `[hotkeys] backlog`, `voice_replay`; `[input] backlog_button`
  (`none` = keyboard and wheel only). Under the parity profile none of
  this exists (the keys do nothing).

### Status at a glance

**Enhanced profile only.** Press **Ctrl+I** (or open **Status at a glance**
in the settings menu) to see what an experienced player keeps track of,
read straight from the game's memory: the game pauses, with its sound, as
for the settings menu, and nothing in the game is changed. **Left / Right**
switch between four tabs, **Up / Down** choose a girl, **Enter** opens or
closes her details, **Escape** or Ctrl+I closes it.

- **Girls**: every girl you have met, her name in the game's own lettering
  (the names Yoshio's "evaluation by the girls" screen prints),
  her **tokimeki** (how much she is in love) and **friendship** as the
  percentages the game uses, her **bomb** meter and value, the number of
  dates that count for her ending, and her **ending score**. A bomb is
  **lit** (`!`) at 60, where Yoshio's screen starts showing the bomb; the
  ticks on the meter are 60, 76 and 91. A score of 500 or more can win her
  ending; `<` marks the girl the game would choose today.
- **Girl**: one girl in detail. Raw tokimeki / friendship and her icon; what
  the bomb will do next (61: a rumour goes round, 76: she confronts you,
  91: Yoshio phones and the bomb explodes, which costs every girl
  friendship and tokimeki; never in the last year); a warning when her
  friendship is under 10 % (the bomb then grows +5 every school week);
  her last date spot (going to the same spot again soon costs you); her
  birthday; and how her ending score is made: 4 x icon + 10 per date (fewer
  than 4 dates: 0), minus her bomb times her weight, plus a bonus or
  penalty for each parameter she likes. For Shiori it shows her checklist
  instead (icon 85, 8 dates, bomb under 51, healthy, and her seven
  parameter levels).
- **Player**: your nine parameters, club, health, this week's activity,
  and **if the game ended today**: whose ending the game's own rule would
  pick (with Rei's phone-call count and Miharu's encounters).
- **Calendar**: today; the next 60 days of the school calendar (exams,
  festivals, the school trip, vacations, holidays), the birthdays the game
  will celebrate (yours, Shiori's, and those of the girls who told you
  theirs), your booked date; and what the game has already scheduled for
  the rest of this week.

The optional **HUD line** (`[enhanced] status_hud = true`, or **Status HUD
line** in the settings menu) shows `Stress n  Bomb: <girl> n` in the top
right corner while you play. Settings: `[hotkeys] status`,
`[enhanced] status`. Under the parity profile none of this exists.

The status page is a spoiler of sorts: it shows numbers the game keeps
hidden. The girls' names are drawn from the game's own font; everything
else is the PC's text.

### How the game works (the guide)

**Both profiles.** The last entry of the settings menu (**Ctrl+O**, then
**How the game works**) is a reference of the game's rules, written from
the game's own code, with its numbers read from your copy of the game:

- **Activities**: what each weekly activity and club does to your nine
  parameters per school day, what changes it (holidays x4, poor days,
  illness), and how poor days are rolled, with your chance for the next
  seven days.
- **Girls**: for each girl, how you meet her (with "You qualify now" when
  you do), what her tokimeki looks at, what her ending needs and your score
  now, the 21 date places with her liking and your chance that she says yes
  on the phone, and what her bomb costs.
- **Events**: the school calendar with every event's dates, exams (and your
  score if the exam were today), clubs and matches, random encounters, the
  bomb events, Rei's and Miharu's stories, the last weeks.
- **Rules**: the week, how friendship fades, moods, answers, phoning, dates
  (what each answer does with your Trivia), who can ask you out now, places,
  illness, how the ending is chosen.

**Left / Right** switch tabs (in a girl's page: the next girl), **Up /
Down** scroll, **Enter** opens a girl or turns a page, **Escape** goes back.
It only reads: opening it changes nothing in your game. Like the status page
it is a spoiler. The evidence for every rule is the game's decompiled code
(toki1-decomp).

### Controls legend

**Both profiles.** Press **Ctrl+H** (`[hotkeys] controls`), or open
**Controls** in the settings menu's Guide section, to see one page with
every PS1 button -- circle, cross, triangle, square, the d-pad, L1, R1, L2,
R2, START, SELECT -- next to the keyboard keys and the controller inputs it
is bound to **now**: it reads the bindings the game is actually using, so
your `[keyboard]` / `[controller]` changes and `controller_layout =
confirm_south` show up there (a name the game did not recognise is left
out). Controller inputs are named as printed on the first connected
controller: Cross / Circle / L2 / Options on a PlayStation pad, B / A / ZL /
+ on a Nintendo pad, "Button 2" and so on for a joystick on the default
mapping (section 4), and Xbox names otherwise or with no controller
connected (A the bottom button, B the right one, X the left, Y the top).
Notes below the table name the controller and say how the mouse works and
where the settings file is.

**Up / Down** scroll, **Enter** turns a page, **Escape** or Ctrl+H closes
it. The game is held while it is open and sees no key: closing it returns
to exactly where you were.

The first time the game starts after it is installed, a note says `Press
Ctrl+H to see the controls`. It is shown once: the PC port records it as
`controls_hint = shown` in the `[state]` section of `settings.ini`; delete
that line to see it again.

### Rewind

**Enhanced profile only.** Made a choice you regret, or missed a line?
**Hold Delete** (or Ctrl+Z): the game stops and the picture runs backwards
through the last 30 seconds of play, about three times as fast as you played
it, with a `<< Rewind -4.2 s` note. **Let go** and the game plays on from the
moment on screen, with its music and voices as they were then. The sound is
silent while you rewind.

- It goes back in steps of 10 game frames (a sixth of a second); the oldest
  point is 30 seconds ago (`[enhanced] rewind_seconds`,
  `rewind_interval_frames`). You cannot go forward again after rewinding.
- Your memory card is not rewound: a save you made in the game (or an
  album you created) stays on the card. Your settings, the dialogue backlog
  and your PC saves are not rewound either.
- Loading a save (F9, the save browser, "continue where you left off")
  starts the rewind history again from that moment.
- A key you were holding together with the rewind key (the Z of Ctrl+Z)
  reaches the game only after you let go of it.
- It costs memory (100-200 MB) and a fraction of a millisecond per frame.
  The settings menu's **Play assists > Rewind** (`[enhanced] rewind =
  false`) turns it off and frees the memory. The key is `[hotkeys] rewind`.
  Under the parity profile the key does nothing.

### Skipping the intro and read dialogue

**Enhanced profile only; both off until you turn them on** (settings menu
**Play assists**, or `settings.ini`).

**Skip intro** (`[enhanced] skip_intro = true`): at power-on the port presses
START for you on the boot movie and again on the title animation -- the same
press that skips them on a PlayStation -- so the title menu appears about a
second after the game starts instead of 34 seconds. It stops pressing at the
title menu and never chooses anything there. The memory-card check between
the two cannot be skipped (it takes under a second). Takes effect at the
next launch. If you press a button yourself, yours wins.

**Skip read text** (`[enhanced] skip_seen = true`): while you **hold the
fast-forward key** (`` ` `` or the right-stick click), the port also presses
circle for you on every dialogue page you have **already read** -- in this
playthrough or an earlier one -- as soon as its "next" arrow appears. A page
you have not read yet is not pressed: the skip stops there, so new text is
never skipped by accident. Choices are never answered.

- "Read" means the page was fully shown and you went on from it (or cut its
  voice short). The port remembers read pages by their text in
  `seen_text.dat` in your saves folder, from every enhanced session, even
  while Skip read text is off; delete the file to forget them.
- The speaker's name is not part of a page's text, so a new player name
  does not make every line new -- but a line that says your name in the
  text counts as new under another name.
- Without the fast-forward key held nothing is pressed. Auto-advance
  (`TOKI_TEXT_AUTO`) is separate and presses on every page
  after a delay.

### Quick text: one press to finish a line

**Enhanced profile only; on by default** (settings menu **Play assists** ->
**Quick text**, or `[enhanced] quick_text` in `settings.ini`).

While a dialogue page is being written out letter by letter:

1. **Press confirm once** (Enter, Space, circle on a controller, or the left
   mouse button -- whichever button the game treats as confirm): the whole
   page appears at once. That press does nothing else: it never also goes
   to the next page.
2. **Press confirm again** while the girl's voice line is still playing:
   the voice stops and the next page starts, with its own voice.
3. Once the voice has ended (the "next" arrow is up), confirm goes on as
   always.

The second press is the game's own voice cut: the original game cuts a
voice line with **cross** when OPTION's voice mode is "cut with the
cross button" (the default), and quick text sends that for you. With voice
mode "always on" the game has no cut, so the second press is ignored until
the line ends, as on a PlayStation; with the voice off there is nothing to
cut. Cross still works as before (it cuts at once, even mid-page).

Quick text acts only on conversation pages (events, dates, phone calls).
Menus, choices (they are never chosen for you), name entry and minigames
get your presses as they are. Holding confirm still makes the text run
faster, as on the console; only a fresh press finishes the page. A page
finished early is still kept in the backlog with its whole text and voice
line, and still counts as read for Skip read text. On a phone page longer
than three lines a press finishes what fits in the window; the game then
scrolls and writes the rest (press again to finish that).

Turn it off for the console's behaviour: the first press does nothing while
a page is being written.

### Saves: the browser, autosave and resume

All of these are PC saves of the whole running game, like a quicksave
(above): they restore the exact moment, load only into the same build, and
never touch your memory card. The game's own saves on the memory card work
exactly as before and are what you keep across updates.

**The save browser.** **Ctrl+S**, or **Browse saves** in the settings menu
(Ctrl+O), lists every PC save, newest first, with its picture, the in-game
date and week, the nine stats (STA stamina, HUM humanities, SCI science, ART
art, SPT sports, TRV trivia, LKS looks, GUT guts, STR stress) and when you
saved it:

- **+ New save** at the top saves the game as it is now as a new entry
  (`Save 1`, `Save 2`, ...). There is no limit but your disk (up to 9999);
  the nine quicksave slots stay for F5 / F9.
- On a save, **Enter** offers **Load**, **Save here** (quicksave slots and
  browser saves only), **Delete** (asks first) and **Cancel**.
- Up / Down choose, Left / Right page by 16, Escape goes back. The game is
  paused while the browser is open; a load or save happens the moment you
  close it with that choice.
- Kinds in the list: `Slot n` (quicksave), `Undo` (slot 0, the game before
  your last load), `Save n` (browser), `Auto n` (autosave), `Ending`
  and `Resume`. A save made by another build is shown greyed and cannot be
  loaded.

**Autosave.** At each week's planning screen (every Sunday night, and on the
first day) the game is saved automatically into `Auto 1`, `Auto 2`,
`Auto 3`, then over the oldest; a note says `Saved autosave 2`. It happens
once per week, when the screen is up and waiting for you, never while the
disc or the memory card is busy, and it does not change the game in any
way. When the ending starts (graduation day), one more save is written
into `Ending`, so you can load it and watch the ending again. Settings:
`[saves]` above, or **Save options** in the settings menu.

**Continue where you left off.** If you quit during a game (the window's
close button or Ctrl+Q), the game is saved as it is (`Saving where you
are...`) and then quits. At the next launch you are asked **Continue
where you left off?** with the picture and date: **Continue** puts you
back there, **Start normally** (or Escape) starts as usual and keeps the
save until you next quit. `--resume` on the launcher continues without
asking. Quitting outside a game (title, album screen) writes nothing; if
you had played this session and left the game, the older resume state is
removed, otherwise (you closed the window at the title or at the question)
it is kept for next time. Closing the window twice quits at once, without
saving.

- Resume works only with the **same build** of the game. After you update
  or rebuild the port, the launch says `Your last session cannot be
  resumed: it was saved by another build of the game` and keeps the old
  state as `resume-old.*`; continue from your memory card save. The same
  holds for every PC save (quicksaves, browser saves, autosaves): use the
  game's own save for anything you want to keep across updates.
- If the game was stopped in a way that could not save (a crash, killing
  the process), the previous resume state is offered; the autosaves are
  your fallback.

Each save is three files in `quicksave/` of your saves folder: `ID.tks` (the
state), `ID.png` (a 160x120 picture), `ID.ini` (date, stats, time, build),
with `ID` one of `slot1`..`slot9`, `slot0`, `save0001`.., `auto1`..`auto9`,
`ending`, `resume`. Deleting the three files deletes the save.

### Extras: endings, gallery, achievements

Achievements are **opt-in** (2026-11-03): `[enhanced] achievements = false` by default, or **Extras → Track achievements** in the settings menu. While off, nothing is unlocked, recorded or announced; the endings list and the gallery work either way.

With the **enhanced** profile the game keeps three records for you across
all your playthroughs, by watching what happens on screen (it never changes
the game):

- **Endings.** Which of the game's **14 endings** you have reached: the
  eleven girls, the two hidden girls (Miharu Tatebayashi and Rei Ijuin) and
  Yoshio's ending, where nobody confesses. Each time the game decides your
  ending on graduation day (1 March 1998) it is recorded with the date and
  time, the girls you had met, her heart / friendship / bomb values and your
  nine stats. Watching an ending again from the game's OPTION screen does
  not count; loading the `Ending` autosave and playing it does.
- **Gallery.** Every picture the game shows you: the backgrounds and date
  spots (out of the disc's 301 pictures) and the animated character event
  scenes (out of 77), each kept the first time it is on screen. Event
  scenes are kept as the screen looked at their brightest moment, text
  window included.
- **Achievements.** 30 of them: your first date, a double date, walking
  home together, meeting a girl, a birthday, a phone call, joining a club,
  the sports festival, the culture festival, the school trip, the Christmas
  party, Valentine's Day, a stat at 999, graduating, graduating with no
  girl's bomb lit (60 or more), every ending, and one for each of the 14
  endings. A note at the top of the picture says `Achievement: ...` when
  one unlocks (and `New ending` / `Gallery: new picture`).

Open them in the settings menu (**Ctrl+O**), under **Extras**:

| Page | What it shows | Keys |
|---|---|---|
| Endings | the 14 endings, the ones you have seen marked with how often; for the chosen one, your last run of it | Up / Down |
| Gallery | how many pictures and event scenes you have of the totals, and a grid of them; Enter shows the chosen one large | arrows choose; Enter / Escape view / back; Left / Right step in the large view |
| Achievements | all 30, unlocked ones marked, what each needs and when you got it | Up / Down, Left / Right by a page |

The records are under your saves folder in `extras/` (`./run_game.sh
--where` prints the saves folder): `extras.txt` (one line per ending,
achievement and picture) and `gallery/` (the pictures as PNG files, which
you can open with any viewer). Delete the folder to start over. They are
kept when you update the game, and quickloads, rewinds and the save browser
never undo them. Under the parity profile the pages still show what you
have, but nothing new is recorded (the pages say so); `TOKI_EXTRAS=1`
turns the tracking on under parity, `TOKI_EXTRAS=0` off under enhanced.

### The settings menu

Press **Ctrl+O** (or the controller's **Guide** button) while playing. The
game pauses, with its sound, and the menu is drawn over the picture. The
game does not see any key you press while the menu is open, and the keys
that close it reach the game only after you let go of them.

| Key | In the menu |
|---|---|
| Up / Down (or your d-pad keys, the controller's d-pad) | choose an entry |
| Left / Right | change it (a slider moves one step, a choice cycles) |
| Enter, Space (or your circle keys / button) | toggle, cycle, open |
| Escape, Backspace (or your cross keys / button) | back; on the main page, close |
| Ctrl+O, Guide, START | close |

| Entry | Setting (`settings.ini`) | Takes effect |
|---|---|---|
| Fullscreen | `display.fullscreen` (also the start mode) | at once |
| Scale | `display.scale`: aspect 4:3 / integer / stretch | at once |
| Filter | `display.filter`: sharp (nearest) / smooth (linear) | at once |
| Scanlines | `display.scanlines`, 0 to 100% in steps of 10 | at once (`CRT on`: a CRT preset replaces it) |
| CRT filter | `display.crt`: off / lite / aperture / shadow mask | at once (`Needs 2x` when the window is too small for it; `Set by env var` when `TOKI_CRT` is set) |
| CRT curvature | `display.crt_curvature` | at once (`CRT off` while no preset is on; `Set by env var` when `TOKI_CRT_CURVATURE` is set) |
| Vsync | `display.vsync` | at once |
| 3D resolution | `graphics.resolution_scale`, 1x to 4x | **on restart**, enhanced profile only (`Restart needed` while the running scale differs; `Set by env var` when `TOKI_RES_SCALE` is set) |
| Sub-pixel 3D | `graphics.subpixel` | at once, enhanced profile only, with 3D resolution 2x or more (`Needs 3D resolution 2x+` otherwise; `Set by env var` when `TOKI_SUBPIXEL` is set) |
| Texture pack | `graphics.texture_pack`: None or a folder in `textures/packs/` | at once when the internal resolution is running (`Restart needed` otherwise), enhanced profile only (`Set by env var` when `TOKI_TEXTURE_PACK` is set) |
| Reload texture pack | (an action) | at once: re-reads the pack folder and every PNG, and refreshes the list of packs |
| Dump textures | `graphics.texture_dump` | at once, enhanced profile only (`Set by env var` when `TOKI_TEXTURE_DUMP` is set) |
| Master, Music, Sound effects, Voice | `audio.*`, 0 to 100% in steps of 5 | at once (heard when the game resumes) |
| Mouse mode | `input.mouse_mode`: absolute / relative | at once |
| Profile | `profile`: parity / enhanced | **on restart**. The note says `Restart needed` while the running profile differs; `--profile`, `TOKI_PROFILE` and `toki.cfg` still win over it. |
| Language | `game.language`: Japanese / English | **on restart** (`On restart` while it differs from the running language; `TOKI_LANG` wins over it) |
| Fast-forward | `enhanced.fast_forward_speed`: 1x to 16x, or Max (0) | at once, in the enhanced profile. `Enhanced only` under parity; `Set by env var` when `TOKI_FF_SPEED` is set. |
| Music packs > Pack | `audio.music_pack`: OFF (original) or a folder found in `music/packs/` when the game started | from the song playing (it restarts from your file's beginning), enhanced profile only (`Enhanced only`; `Set by env var` when `TOKI_MUSIC_PACK` is set; `NO PACKS FOUND`) |
| Music packs > Dump songs | `audio.music_dump` | at once (a take begun mid-song is kept only while it is the longest of that song) |
| Music packs > SoundFont | `audio.soundfont`: OFF (original) or a `.sf2` found in `music/soundfonts/` when the game started | at once (a song playing switches at its next notes), enhanced profile only (`Enhanced only`; `Set by env var` when `TOKI_SOUNDFONT` is set; `No .sf2 found`; `Cannot load`) |
| Sound quality > Interpolation, Reverb | `audio.interpolation` (gaussian / cubic / sinc), `audio.reverb` (original / high precision) | at once, enhanced profile only (`Enhanced only` under parity) |
| Sound quality > Output rate | `audio.output_rate`: 44100 / 48000 Hz | **on restart**, enhanced profile only |
| Backlog | `enhanced.backlog`: on / off | at once, enhanced profile only (`Enhanced only` under parity). `Show backlog` opens it (the note counts the pages), `Replay last voice` is Ctrl+R. |
| Status at a glance | the status page (Ctrl+I, above) | enhanced profile only (not listed under parity) |
| Status HUD line | `enhanced.status_hud`: on / off | at once |
| How the game works | the guide (above) | both profiles |
| Controls | the controls legend (Ctrl+H, above) | both profiles |
| Browse saves | the save browser (Ctrl+S, above) | |
| Play assists: rewind, skip intro, skip read text, quick text | `enhanced.rewind`, `enhanced.skip_intro`, `enhanced.skip_seen`, `enhanced.quick_text` | rewind, skip read text and quick text at once, skip intro at the next launch; enhanced profile only (`Enhanced only` under parity; `Set by env var` when `TOKI_REWIND` / `TOKI_SKIP_INTRO` / `TOKI_SKIP_SEEN` / `TOKI_QUICK_TEXT` is set). The note of Skip read text counts the pages read so far. |
| Extras: Endings, Gallery, Achievements | the extras pages (above) | |
| Save options: autosave, autosaves kept, autosave at ending, resume after quit | `saves.autosave`, `saves.autosave_slots` (1 to 9), `saves.autosave_ending`, `saves.resume` | at once |

Closing the menu writes the entries you changed into `settings.ini` and
shows `Settings saved`. In an automated run (section 2) nothing is written:
the menu says `Automated run: changes not saved` and the change lasts until
the run ends. Bindings and the other keys are still edited in the file.

### Mouse

Move the mouse and the game sees a PlayStation mouse on port 1: left click
confirms, right click cancels, and the game's own option swaps them. By
default (`mouse_mode = absolute`) the game's pointing hand follows your
pointer: its fingertip sits under the pointer, and the OS cursor is hidden
while it is over the picture. The game still moves its own cursor (by the
amounts a real mouse would report), so its speed option and its edge
limits apply: the fingertip stops about 12 pixels (of 320) from the left
edge, 17 from the right and 20 from the top and bottom, as on the console. Press any key or
controller button to hand port 1 back to the pad. Some screens accept only
a pad, as on the console.

The game switches its menus to the free-moving hand when it sees a mouse
and, on the console, leaves them that way. In the enhanced profile your
pad's menu mode (by default the d-pad steps through the options) comes back
when you return to the pad. SELECT, or "menu mode" in the game's Options,
changes it at any time.

**The hand moves the wrong way?** That was `relative` mode under WSLg (and
other remote or virtual displays): their pointer reports positions, not
motion, and SDL turns those into motion with the sign reversed. Use the
default `mouse_mode = absolute`.

### Music packs

**Enhanced profile only.** A music pack replaces the game's songs with your
own recordings -- an OST rip, a re-orchestration -- at whatever quality you
have. The game itself does not notice: its sequencer keeps playing every
song note for note (the game times things off it), the port only leaves
those notes out of what you hear and plays your file instead. Sound effects
and voices are untouched. A song with no file in the pack plays as the
original.

**1. Find out which song is which.** Every song has a *track id* such as
`seq-1e832945` (the title screen's song): a fingerprint of the song's notes,
the same whichever screen plays it. Three ways to see them:

- Turn on *Dump songs* (settings menu, MUSIC PACKS page; or `music_dump =
  true` under `[audio]`) and play: each song you hear is written to
  `music/dump/<track id>.wav` as the PlayStation plays it (the longest take
  of each is kept), and `music/dump/heard.jsonl` records where you heard it.
- `python3 pc/tools/music_tracks.py --list-music` (in a source checkout)
  lists all 176 songs on your disc with their sound-bank name (`DAYS#0`,
  `NARR_VOL#0`, ...), length and loop points; add `--music-dir
  ~/.local/share/tokimeki-pc/music` to include where you heard them and the
  dumps. `--out tracks.md` (or `.tsv` / `.json`) writes it to a file.
- With a pack selected the game prints a `[MUSIC] ... start acc 0
  seq-xxxxxxxx` line each time a song starts.

**2. Make the pack.** A pack is a folder in `music/packs/` (its name is
what the menu shows). Put one file per song in it, named by track id:

```
music/packs/my-ost/
    seq-1e832945.ogg        <- the title song
    seq-1e832945.ini        <- optional: its loop points
    seq-d2ab1e54.flac
    seq-ffb80a52.wav
```

`.ogg` (Ogg Vorbis), `.flac` and `.wav` (16-bit, 24-bit or float PCM) are
read, at any sample rate, mono or stereo. Use the folder in the settings
menu (MUSIC PACKS > PACK) or `music_pack = "my-ost"` under `[audio]`; a
change applies from the next song.

**3. Loop points.** Most songs loop for ever. While the game is still
playing the song, your file loops too: from its loop start to its loop end,
or the whole file when it has no loop points. When the game's own song
reaches its end by itself (jingles), your file plays to its end and stops.
Loop points, in samples of the file (first match wins):

- a sidecar `<track id>.ini` next to the file:
  ```ini
  loop_start = 441000     ; sample where the loop begins
  loop_end   = 5292000    ; sample after the last one in the loop
  loop       = true       ; false: never loop this file
  gain_db    = -2.5       ; louder / quieter than the file, in dB
  ```
- `LOOPSTART` with `LOOPEND` or `LOOPLENGTH` tags (Vorbis comments in
  `.ogg` / `.flac`; the RPG Maker convention);
- a WAV `smpl` chunk's first loop.

`--list-music` shows where the original song's own loop starts and ends
(in seconds of the song) -- a guide to cutting your file.

**What follows the game.** Your file starts on the same frame as the
song, pauses and resumes with it, stops when the game stops or closes it
(a 20 ms fade avoids a click), and follows the game's fades: the song's
volume relative to where it started, and the console's main volume.
`[audio] music` and `master` set its level. Fast-forward mutes it with the
rest of the sound.

**Not covered:** the streamed music track `BGM378.XA` (it is disc audio,
not a sequenced song; no screen measured so far plays it) and the boot
movie's sound.

### SoundFont mode

**Enhanced profile only.** A SoundFont is a bank of instrument recordings
(a `.sf2` file). In SoundFont mode the game's music is played with its
instruments instead of the PlayStation's: the same notes, at the same
moments, with the game's own tempo, fades, pauses and reverb -- only the
instruments change. Sound effects and voices are untouched, and the game
does not notice (its sound chip keeps playing every note; you just do not
hear the music voices).

**The port ships no SoundFont** (they have their own licences). Free
General MIDI SoundFonts that work well, by name and licence -- download one
from its author's page:

| SoundFont | Size | Licence |
|---|---|---|
| GeneralUser GS (S. Christian Collins) | ~30 MB | its own permissive licence: free to use, also commercially |
| FluidR3_GM (Frank Wen) | ~140 MB | MIT |
| MuseScore_General (S. Christian Collins) | ~200 MB | MIT |
| TimGM6mb (Tim Brechbill) | ~6 MB | GPL-2.0 |

Use the `.sf2` file (a compressed `.sf3` is not read). The port holds the
whole font in memory as 32-bit samples, about twice the file's size; a
large font takes a second or two to load when you choose it.

**1. Install.** Put the `.sf2` into `music/soundfonts/` (under the data
folder, next to `music/packs/`) and choose it in the settings menu (MUSIC
PACKS > SOUNDFONT), or `soundfont = "GeneralUser GS.sf2"` under `[audio]`.
OFF returns to the original music at once.

**2. Which instrument plays what.** The game's instruments are its own
sound banks (VABs), not General MIDI, so the port guesses: for each of the
game's instruments it looks at the instrument's notes and samples (a drum
kit, a bass, a sustained sound that swells in slowly -- strings -- or starts
at once -- flute, reed or brass by how bright it is --, a decaying sound --
piano, mallets, guitar) and for a drum kit at each drum's sound (kick,
snare, hi-hat, cymbal, tom). Every guess is written to
`music/soundfonts/<name>.auto.map` and to the log (`[SF2] vab-30d48b05
prog 2 -> 0:12 (...)`). To change one, copy its lines into
`music/soundfonts/<name>.map` (the same name as the `.sf2`) and edit them:

```ini
; music/soundfonts/GeneralUser GS.map
[global]
gain_db = -3            ; all SoundFont music 3 dB quieter
velocity = 100          ; how hard notes are played (1..127; the font's layers)

[vab-30d48b05]          ; one of the game's sound banks (from the log / auto.map)
prog 0 = 0              ; its instrument 0 plays General MIDI program 0 (piano)
prog 1 = 0:48           ; bank:program -- strings
prog 2 = drums          ; the drum kit (drums:25 = kit 25)
prog 2 key 72 = 38      ; that kit's note 72 plays the snare
prog 3 transpose = -12  ; an octave down
prog 3 gain_db = 4      ; louder
prog 4 = off            ; keep the PlayStation's own instrument for this one

[seq-1e832945]          ; one song (its track id, as for music packs):
prog 1 = 0:73           ; there, instrument 1 is a flute
```

A song's section wins over its sound bank's; anything not in your map is
guessed. The map is read when the SoundFont is chosen (choose OFF and the
SoundFont again in the menu to reload it).

**With a music pack.** A song that has a file in your music pack plays the
file; the others play through the SoundFont.

**Not covered:** the streamed `BGM378.XA` and the movie sound (disc audio);
the song dump (`music_dump`) is paused while a SoundFont plays.

### Sound quality

**Enhanced profile only;** the defaults are the console's sound. Settings
menu: SOUND QUALITY, or `[audio]` in `settings.ini`.

- **Interpolation.** The PlayStation plays its samples through a
  smoothing filter (`gaussian`) that takes some treble away -- the slightly
  muffled sound of the console. `cubic` keeps more of it at the same cost;
  `sinc` keeps the most and also keeps high notes clean (it filters a
  sample played far above its recorded pitch instead of letting it alias).
  Measured on a test tone at a quarter of its sample's rate: gaussian
  -3.2 dB against sinc, cubic -0.4 dB.
- **Reverb.** `hq` runs the console's own reverb in floating point: the
  same rooms and echo times, without its 16-bit rounding (smoother, quieter
  tails). The difference is subtle.
- **Output rate.** If your sound card runs at 48 kHz (most do on Windows),
  `48000` lets the port convert the game's 44.1 kHz sound with a
  high-quality filter instead of the system's converter. Takes effect when
  the game starts.

## 4. Controllers

Controllers work without setup, over USB or Bluetooth:

- **Xbox** 360 / One / Series pads (XInput, RawInput or Windows.Gaming.Input
  on Windows), and the many pads that present themselves as one.
- **PlayStation**: DualShock 4 and DualSense (SDL's own drivers, wired or
  Bluetooth, no DS4Windows needed); DualShock 3 through its Windows driver
  (DsHidMini) or on Linux.
- **Nintendo**: Switch Pro, a pair of Joy-Cons (used together as one pad),
  NSO SNES / N64 / Mega Drive pads, GameCube pads on the USB adapter.
- **Steam Deck** and Steam: through Steam Input, which presents an
  Xbox-style pad. A Steam Controller without Steam: set the environment
  variable `SDL_JOYSTICK_HIDAPI_STEAM=1`.
- 8BitDo, Stadia, Luna, Shield, PowerA, PDP, Hori and the other pads in
  SDL's database.
- **Any other USB / DirectInput pad, arcade stick or clone** SDL has no
  mapping for gets a default one (below) instead of being ignored.

Controllers can be plugged in and out at any time, also while the settings
menu is open. The log prints `[PAD] controller connected: NAME (mapped)`
or `(default mapping)` and `[PAD] controller disconnected: NAME`. The
keyboard, the mouse and every controller work at the same time; while the
window is in the background, controller presses are ignored, as keys are.

Default (`positional`) layout. The buttons go by **position**, whatever is
printed on them:

| Button position | Xbox / Deck | PlayStation | Nintendo | PS1 button |
|---|---|---|---|---|
| south | A | ✕ | B | cross (cancel) |
| east | B | ○ | A | circle (confirm) |
| west | X | □ | Y | square |
| north | Y | △ | X | triangle |
| shoulders | LB / RB | L1 / R1 | L / R | L1 / R1 |
| triggers | LT / RT | L2 / R2 | ZL / ZR | L2 / R2 |
| Menu / View | Start / Back | Options / Share | + / - | START / SELECT |
| d-pad, left stick | | | | d-pad |
| Guide (Xbox, PS, Home); PS touchpad click | | | | settings menu |
| left stick click | | | | dialogue backlog (enhanced) |
| right stick click (hold) | | | | fast forward (enhanced) |

On a PlayStation pad this is exactly the console; on a Nintendo pad,
confirm is A, as on a Nintendo console. On an Xbox pad or Steam Deck, B
confirms and A cancels, as in a Japanese import. If you prefer the bottom
button to confirm, set:

```ini
[input]
controller_layout = confirm_south
```

The controls legend (Ctrl+H) names the buttons as printed on the first
controller connected. The settings menu and the backlog move with the
d-pad or the left stick; the bottom/right buttons (as bound to cross and
circle) go back and choose. On Windows the Xbox button may open the Xbox
Game Bar too: turn that off in Windows Settings > Gaming > Xbox Game Bar,
or use another `menu_button`. Analog triggers press L2/R2 past
`trigger_threshold`; triggers that are only buttons (Switch, most clones)
work too. The PS1 pad of this game has no vibration, so nothing rumbles.

**A second pad on port 2.** The game reads a second PS1 controller in a
few places (among them its START+SELECT+L1+R1 reset, which on port 2
restarts at a different point). With `port2_controller = true` in `[input]`
the first controller connected is player 1 (port 1, together with the
keyboard and the mouse) and the second is player 2 (port 2); pads with
player lights show 1 and 2. When player 1 is unplugged, player 2 becomes
player 1. Leave it off (the default) if the PC shows one pad twice (Steam
Input or DS4Windows running alongside the real pad): then every controller
is player 1.

**A controller that is not recognised, or has the wrong buttons.** A
device SDL does not know gets this default, which suits most
PlayStation-style USB pads and arcade sticks (button numbers as Windows'
"Set up USB game controllers" shows them):

| Button | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 / 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| PS1 | triangle | circle | cross | square | L1 | R1 | L2 | R2 | SELECT | START | L3 / R3 | Guide |

and the first hat (or the first stick) is the d-pad. The log prints the
mapping it used:

```
[PAD] no controller mapping for "USB Gamepad" (12 buttons, 4 axes, 1 hats): using the default one; ...
[PAD]   03000000341200007856000000000000,USB Gamepad,y:b0,b:b1,a:b2,x:b3,...,platform:Windows,
```

To change it, copy that line into a file named `gamecontrollerdb.txt` in
the settings folder (or beside the program) and edit the numbers: `b:b1`
means "SDL's b, the right button, which is circle, is button 2" (`bN` counts
from 0). Every line needs its `platform:` field. You can also put lines
from the community [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)
there (the file is not shipped with the game). SDL also reads the
environment variables `SDL_GAMECONTROLLERCONFIG` (mapping lines) and
`SDL_GAMECONTROLLERCONFIG_FILE` (a file). `TOKI_JOYSTICK_FALLBACK=0` turns
the default mapping off. Remapping the PS1 buttons themselves (for every
controller) is `[controller]` (section 2).

## 5. Troubleshooting

| Problem | Fix |
|---|---|
| Setup: "Put your disc image in the disk folder" / "The disk folder is empty" / "No disc image in the disk folder" | Make `disk/` beside the program and put the `.cue` and both `.bin` files there, then press R; or drag the `.cue` onto the window, or press Enter to choose it. |
| Setup: "A file the .cue needs is missing" | The screen lists each file the `.cue` names (Found / Missing) and what the folder holds. Rename the `.bin` files to exactly those names, or fix the `FILE` lines in the `.cue`. |
| Setup: "Track 1 has the wrong size" | Found vs expected bytes are shown. Smaller: an incomplete download or copy; copy it again. Other sizes: another release. |
| Setup: "This is not the supported release" | The Track 1 SHA1 found and expected are shown: another revision (Rev 0), a patched or translated image, or a damaged dump. Only Rev 1 (table above) works. |
| Setup: "This is a cooked ISO, not a raw BIN/CUE" | A 2048-byte-sector `.iso` has lost the voice, music and movie sectors. Dump the disc again as raw BIN/CUE (2352-byte sectors, both tracks). |
| Setup: "CHD / PBP / ECM images are not supported" | CHD: `chdman extractcd -i game.chd -o game.cue`. ECM: `unecm`. PBP cannot be turned back into the disc; use a BIN/CUE dump. Archives (zip, 7z, rar): extract first. |
| Setup: "This is not a disc image" / "This is the audio track (Track 2)" | Choose the `.cue` (or the Track 1 `.bin`). |
| Setup: "Track 2 (CD audio) is missing" / "... differs" | Press C to continue without it: the game never plays that track. Or add a good Track 2 `.bin` and press R. |
| Setup: "Not enough free disk space" | Needed and free space are shown (about 1.5 GB while extracting). Free some space or move the folder, then R. |
| Setup: "The game data cannot be written" | Move the folder somewhere you own (Desktop, Documents), or add `user-data.txt` beside the program to use the per-user folder. |
| At launch: "The game data is damaged or incomplete" | Each launch checks every data file's presence and size. Enter moves the damaged files into `data.damaged-N` (never deleted; the log and `data/en/` stay) and extracts again from `disk/`. |
| Anything else / to check everything | Read `data/tokimeki-pc.log` (`.log.old` is the run before). Settings menu (Ctrl+O) > GAME DATA AND ABOUT > VERIFY GAME DATA, or `tokimeki-pc --check`, re-hashes the disc and every extracted file and writes a report to the log. |
| `Build failed` | Install a compiler and SDL2 dev files (`build-essential libsdl2-dev`). |
| No window appears (Linux) | The launcher warns when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set; start it from a desktop session. On WSL, WSLg works; `./run_game.sh --vcxsrv` uses an X server on Windows instead. |
| Window too big or off-screen | Delete the `window_*` lines under `[state]` at the end of `settings.ini`, or set `window_width` / `window_height` and `remember_window = false`. |
| A key does nothing | Check the console for `unknown key` warnings from your `[keyboard]` lines; names are SDL names (section 2). |
| Controller not detected | Check `tokimeki-pc.log` for `controller connected`; try another USB port or Bluetooth mode, and check `[input] controller = true`. A device the operating system does not show as a game controller at all (some pads in a keyboard mode, a Steam Controller without Steam) cannot be seen; see section 4. Keyboard and mouse keep working without one. |
| Wrong buttons on the controller | On every pad the buttons go by position (section 4). See `controller_layout`, bind `[controller]` lines yourself, or, for a pad on the default mapping, edit its line in `gamecontrollerdb.txt` (section 4). |
| One controller does nothing with `port2_controller = true` | It is player 2. Steam Input or DS4Windows may show one pad twice; set `port2_controller = false`. |
| Too loud or too quiet | `[audio]` in the settings, the settings menu (Ctrl+O), or Ctrl+= / Ctrl+- while playing. |
| Mouse hand moves the wrong way | Set `mouse_mode = absolute` in `[input]` (the default since 2026-11-01). |
| Music cut off or two songs at once at a scene change (enhanced) | Fixed on 2026-11-01 (`CD_WAIT_MUSIC_FADE`); update your build. |
| Two songs seem to play at once somewhere else | Send `tokimeki-pc.log`: its `[MUSIC]` lines (one per song start / stop) show which songs the game played and when. |
| Voices or the movie's sound crackle or drop out | Fixed on 2026-11-01 (a jitter margin on disc audio and steadier frame pacing); update your build. |
| Music or sound effects skip notes, "as if a channel were missing" (both profiles, most on Windows) | Fixed on 2026-11-02 (notes keyed in two frames in a row were lost between two audio buffers); update your build. If it still happens, send `tokimeki-pc.log`: its `[SPU] audio driver` / `audio device` lines and the `[AUDIO] health` lines (late buffers, notes lost, disc-audio gaps; a line at most every 10 s, only when something went wrong, and the totals at exit) say where. |
| Lost my saves after rebuilding | Builds before 2026-11-01 kept them in `pc/build/saves/`, which a clean rebuild deletes; restore that folder's `card1.mcr` from your backup into the saves folder `--where` prints. Since then the launcher keeps them outside `pc/build` and copies an old card across once. |
| "Cannot be resumed: saved by another build" at launch | The port was updated or rebuilt since you quit; PC saves load only into the build that wrote them. Load your memory card save from the title. |
| No "Continue where you left off?" | You quit outside a game, `[saves] resume` is off, or you started the executable in an automated way (section 2). |
| Want the defaults back | Delete `settings.ini`. |
| Settings seem ignored | Check the first console lines. `[SETTINGS] automated run (...)` means a tool or test variable is set in your environment (section 2). `[SETTINGS] loaded PATH` shows which file was read. |

The program's output is verbose: it is the developer log and is safe to
ignore. Without a console (the Windows program, or Linux started from a
file manager) it goes to `data/tokimeki-pc.log` (capped at 64 MB), after the
setup's own lines: the folders chosen, the files checked and their hashes
found vs expected. `PC_DEBUG=pad` adds input diagnostics, which help when
reporting a controller problem.

## 6. Texture packs: dump, upscale, load

A texture pack replaces the game's pictures -- character portraits,
backgrounds, event pictures, menus and icons -- with your own versions, for
example sharper ones made with an AI upscaler. It works like texture packs
in other emulators. It needs the **enhanced profile** (`profile =
enhanced`, section 2). The game itself is untouched: it still sees and
reads its original pictures, so it plays exactly the same; only what you
see on screen changes.

You must make the pack yourself from your own copy of the game. Never share
dumped pictures: they are the game's artwork.

### Step 1: dump the pictures

1. Set, in `settings.ini` (section 2):

   ```ini
   profile = enhanced

   [graphics]
   texture_dump = true
   ```

   or switch **Dump textures** on in the settings menu (Ctrl+O) while
   playing.
2. Play through the parts of the game you want to change. Every picture the
   game draws is saved once, the first time it appears, into the dump
   folder: `textures/dump/SLPS-00065/` in the data folder
   (`./run_game.sh --where` prints it as `Textures:`). Already-dumped
   pictures are never written again, so you can dump over several sessions.
3. Switch dumping off again when you are done (it costs a little time).

The dump folder holds:

- PNG files named like `t8-6932befbfaf4ee77-d60cb5a93db01015-128x128.png`.
  **Do not rename them**: the name is how the game finds your replacement.
  It is made of the kind (`t16` = a full-colour picture, `t8` / `t4` = a
  256- / 16-colour picture with its palette), a code for the picture's
  content, a code for its palette, and its size in pixels.
- `index.csv`: one line per file, with the scene number and the frame where
  it first appeared, to help you find what is what. Open it in a
  spreadsheet, or run
  `python3 pc/tools/texture_pack_tool.py list <dump folder>` for a summary.

What you will find (about 190 pictures in the first two in-game months):
character portraits as 128x128 pieces, backgrounds and event pictures as
320x240, 320x160 or 320x88 images, and 128x128 sheets of menu parts and
icons. A menu sheet appears several times, once per colour palette the game
draws it with; each copy is used for the parts drawn in that palette. A
handful of images look like noise: the game drew them that way for a few
frames (for example, while a background was still loading). Leave those out
of your pack. Text is not dumped by default (each character is a separate
tiny picture).

### Step 2: upscale

1. Copy the PNGs you want to change into a new folder (keep the names).
2. Upscale or redraw them with any tool: an AI upscaler (Real-ESRGAN,
   waifu2x, chaiNNer, Upscayl, ...), or by hand.
   - Use a **whole factor**: 2x or 4x. Larger than 4x gives no more detail
     (the game draws at 4x at most). Other sizes work but are resampled.
   - Keep the **transparency** (the alpha channel). Transparent pixels are
     see-through in the game; keep them transparent, or the picture gets a
     box around it. Partly transparent pixels (soft edges from the
     upscaler) are blended with what is behind them, so smooth edges are
     fine. Most AI upscalers have an option to keep or upscale the alpha
     channel; turn it on. If yours drops alpha, upscale the alpha
     separately and recombine, or the edges of portraits and icons will
     show their background colour. Backgrounds and event pictures have no
     transparent pixels.
   - Keep the **same layout**: the game takes pieces out of these pictures
     by position, so do not crop, shift, or change the aspect ratio.
   - Keep the **colours** close to the original if you do not want a
     different look: the game tints and fades some pictures itself, and
     that still works on yours.
   - Save as PNG (8-bit RGBA or RGB).
3. Put the results in `textures/packs/<a name you choose>/`, for example
   `textures/packs/hd/`.
4. Optional check:
   `python3 pc/tools/texture_pack_tool.py validate <pack folder> --dump <dump folder>`
   reports misnamed files, sizes that are not whole multiples, and files
   that match no dumped picture.

### Step 3: load the pack

1. Set in `settings.ini`:

   ```ini
   [graphics]
   texture_pack = "hd"
   ```

   or choose it under **Texture pack** in the settings menu (Ctrl+O).
2. Start the game. The console says `[TEXTURES] texture pack ...: N
   replacement(s), scale 4x`. The game now draws at the pack's scale (or at
   `resolution_scale`, whichever is higher), so a window or screen at least
   that big shows the detail; screenshots are saved at that size too.
3. Editing the pack while playing: change or add PNGs, then choose **Reload
   texture pack** in the settings menu. The new pictures appear the next
   time the game draws them.

Anything the pack does not contain is drawn as the original. If a
replacement does not show:

| Symptom | Cause |
|---|---|
| Nothing changes at all | Is the profile `enhanced`? Does the console list the pack and a number above 0? With `TOKI_TEXTURE_PACK` set in the environment, that wins over the settings. |
| One picture is not replaced | Its name differs from the dumped one (renamed, or the upscaler added a suffix such as `_out`). Run `validate`. |
| A picture is replaced in one scene but not another | The game draws it there with a different palette (a different `t8`/`t4` name, often for a fade or a colour variant). Dump that scene too and replace that file as well. |
| A box or fringe around portraits and icons | The upscaler dropped or blurred the alpha channel. |
| Briefly blurry during a scene change | Transitions use a captured copy of the screen at the console's resolution; this is expected. |
| `Restart needed` next to Texture pack | The pack was chosen while the game was running at 1x; restart to draw it at its scale. |
