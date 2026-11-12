# State machines of the PC layer

Audit of 2026-11-03: every piece of PC-port code we wrote (`pc/hal/**`, the
PC-layer files of `pc/src/**`, `src/main/pc-port/**`, `pc/include/**`; not
the matched PS1 code and not the generated `pc/src/overlay_*.c`) was read for
**implicit state machines**: behaviour that depends on a combination of
flags, counters, sentinels or "armed / pending / active" variables written in
several places, instead of one state variable with defined transitions.

For each machine this file gives the states and transitions, the owner file,
the threads that touch it, its invariants and its tests. It then lists the
bugs found (fixed, with the test that reproduces each one and its negative
control) and the fixes proposed for the audio and UI files, and ends with the
risks that remain.

Line numbers drift. The text names functions; `grep` finds them.

Contents:

1. [Method and threads](#1-method-and-threads)
2. [The cross-cutting machine: a restore of the machine](#2-the-cross-cutting-machine-a-restore-of-the-machine)
3. [Inventory](#3-inventory) (one section per machine)
4. [Bugs found and fixed](#4-bugs-found-and-fixed)
5. [Proposed fixes in the audio and UI files](#5-proposed-fixes-in-the-audio-and-ui-files)
6. [Remaining risks](#6-remaining-risks)
7. [Tests](#7-tests)

---

## 1. Method and threads

Three parallel read-only audits covered (a) the menus, the setup screen,
presentation, input and language, (b) the play features (quicksave,
autosave, rewind, quick text, backlog, extras, writers, watchdog), and (c)
audio, CD, video, the GPU-side PC features, overlays and system code. Each
candidate bug was then reproduced on the port, or rejected, before anything
was changed.

Threads in the port:

| Thread | Created by | Runs |
|---|---|---|
| game | `main` | game code, the SDL event pump (`hal_frame_begin`, `hal_poll_quit`, the pause loop), every VSync, the toolkit boundary (`dbg_frame_boundary`, on its own stack), the CD and sound workers |
| audio | SDL (`spu.c` `audio_callback`) | the mixer: SPU core, register-queue replay, XA FIFO, music pack streams, voice replay. Everything it reads is written under `spu_hal_audio_lock` / `SDL_LockAudioDevice` |
| quicksave writer | `quicksave.c` `qs_save_now` (pthread, one per save, joined) | writes `ID.tks/.png/.ini` from a capture that is already a copy |
| rewind coder | `rewind.c` `setup` (pthread, detached, one per process) | codes HEAD xor SPARE under `g_mu` / `g_cv` |
| card writer | `card_writer.c` (pthread, detached) | writes `card1.mcr` from an image copy |
| file writer | `async_io.c` (SDL thread, detached) | PNG/texture/screenshot jobs; queue under `g_lock` |
| setup worker / verify | `setup_screen.c` (SDL threads) | the first-run extraction and the menu's verify job |
| watchdog | SIGALRM (Linux) or a thread (Windows) | longjmps out of a hung game frame |

No machine in this audit had a lock bug in the narrow sense (a shared
variable written without its lock). The real defects were of three other
kinds: **state that a restore wrote back but should not have** (host
resources inside save states), **observers that a restore path did not
tell**, and **edges** (an event delivered twice, a flag not cleared on one
exit path).

---

## 2. The cross-cutting machine: a restore of the machine

A save state (`pc/hal/dbg_state.c`) records the executable's data minus its
**KEEP** rules, plus PS1 RAM, the MMIO window and the dead game stack. Every
PC-layer static that is not KEEP is therefore *machine state*: a load or a
rewind writes it back. Every static that is KEEP is *session state*: it
survives a load, and if it watches the machine, it must be **told** that the
machine changed.

### 2.1 Save-state ownership (KEEP)

Rule: host resources (pointers to heap, threads, mutexes, SDL objects,
`FILE*`), the player's configuration and what the player's hands are doing
now are the session's. They must be KEEP. Game-visible device models are
the machine's and must not be.

Fixed 2026-11-03 (bug **S1**, section 4). These were saved and written back:

| Object | What it holds | Effect of writing it back |
|---|---|---|
| `async_io.c` (whole file) | the file writer's `SDL_mutex*`, `SDL_cond*`, thread, job queue | a state from another process: the next texture dump / screenshot / gallery picture locks a dead mutex. Measured: hang in `pthread_mutex_lock`, then the watchdog's crash loop |
| `gpu_texrep.c` (whole file) | heap tables (`g_src`, `g_owner`, `g_oval`, `g_dumped`, `g_pend`, `g_pack*`), `g_cfg.index` (`FILE*`) | use-after-free after a table grew; another process's pointers |
| `gpu.c:g_hires`, `gpu.c:hires_tex/_w/_h`, `gpu_hires.c:pixels/pixels_scale` | the shadow VRAM's heap buffers, the N× display texture | another process's pointers (crash), or another scale |
| `g_texrep_on`, `g_texrep_replace`, `g_extras_gpu` | hook switches decided by this session | a pack / gallery hook switched by an old state |
| `pad.c` (whole file) | key bindings, mouse mode, stick thresholds, cursor hidden, steering, the text assist's mouse pulse | edit the bindings, relaunch, resume: the old bindings for the whole session; MOUSE MODE silently reverts after a quickload; the OS cursor can stay hidden |
| `status_view.c` (whole file) | the status page's id and latch | a state from the other profile: Ctrl+I dead, or the wrong page id |
| `pad_device.c:g_mouse_left/right`, `g_hold_off` | the mouse buttons held now; the hold-off a closed menu armed | a load from the save browser hands the still-held confirm key to the loaded game; a mouse button "held" after a load |
| `spu.c:g_held` | the device hold the rewind / pause loop set | written to 0 under the rewind loop (harmless only because the device is paused there) |

Kept as machine state on purpose (documented, not changed):

* `lang.c` (`g_hal_lang_en`, `g_en_root`): the language decides which text
  data the game's memory holds. A state carries its language with its
  memory; keeping the flag would make the two disagree. A load into a
  session of the other language therefore switches the session's language
  (the menu still shows the setting; it applies on restart).
* `quick_text.c`, `backlog_capture.c`: they watch the machine and are
  restored with it (their file comments say so).
* `cd_audio.c` `g_have_window` / `g_window_end`: the deterministic mixer's
  window; parity replays need it restored. A state from a parity run loaded
  into a real-time session is a developer path (risk R6).

Check: `savestate_tool.py audit STATE` lists every saved word that holds a
host address. On a state saved under the enhanced profile with internal
resolution 2× and texture dumping, it now lists only `g_vram` (pixel values)
and `gpu_pgxp.c:g_table` / `g_tri` (coordinate fractions), which are value
coincidences (`test_state_machines.HostResourcesNotInStates`).

### 2.2 After a restore: one list of observers

Before 2026-11-03 each restore path told its own subset of observers:

| Path | trace clock | extras | pacing | autosave | rewind buffer |
|---|---|---|---|---|---|
| `dbg_state_load` (inside every file load) | yes | yes | – | – | – |
| quickload / browser / resume (`qs_load_now`) | yes | yes | yes | yes | yes |
| toolkit `load`, F9 (debug), `PC_LOADSTATE` | yes | yes | **no** | **no** | yes |
| rewind (`after_restore`) | yes | **no** | yes | yes | (itself) |

In addition, `dbg_state_load` returned −1 on an overlay mismatch after it had
already restored the machine, so `qs_load_now` skipped its hooks.

Now there is one function, `dbg_state_after_restore(how)` in `dbg_state.c`,
with `how` = `DBG_RESTORE_FILE` or `DBG_RESTORE_REWIND`:

| Order | Observer | File | What it does |
|---|---|---|---|
| 1 | `trace_after_load` | trace.c | the absolute input's VBlank clock follows the loaded clock |
| 2 | `extras_after_load` | extras.c | the ending detector re-arms from the loaded moment; gallery watchers reset; `g_extras_gpu` re-decided |
| 3 | `gpu_hires_after_restore` | gpu_hires.c | every shadow block whose native value changed is refilled at 1× |
| 4 | `text_assist_after_load` | enhance.c | the auto-advance wait, the press in flight and the arrow seen are dropped |
| 5 | `hal_pacing_resync` | system.c | wall pacing starts a new schedule from the loaded clock |
| 6 | `sa_after_load` | save_auto.c | an armed save is dropped; the loaded week counts as saved |
| 7 | `spu_hal_after_restore` | spu.c | the real-time register queue's old writes are dropped (not applied); a SoundFont's notes are silenced (C1) |
| 8 | `music_pack_after_load` | music_pack.c | the dump's take ends; the next VBlank identifies every open song again by its bytes (C4) |
| 9 | `rw_after_load` (file only) | rewind.c | the rewind buffer described another time line |

`dbg_state_load` calls it right after the copy (before the overlay check);
`rewind.c` calls it after a restore. Nothing else calls the observers.
`PC_RESTORE_LOG=1` prints `[RESTORE] file: trace extras hires assist pacing
autosave audio music rewind` (or `rewind: ...` without the last one), and
`test_state_machines.EveryRestoreTellsEveryObserver` checks all four paths.

**Invariant:** a new session-side observer of the machine is added to this
function, and to `KEEP_FILES` or `KEEP_SYMS` if it holds host resources.

---

## 3. Inventory

Notation for the transition tables: one row per transition, `event (site) →
new state`. "Restored" means the variables are machine state (written back
by a load or rewind); "kept" means they are KEEP (session state).

### 3.1 Autosave, ending save, resume and quit — `pc/hal/save_auto.c` (REFACTORED)

Threads: game only (`sa_frame` from `hal_frame_begin` inside VSync,
`sa_boundary` from `qs_boundary` at the toolkit boundary, `sa_defer_quit`
from `hal_request_quit`). Kept.

Before: `g_week_armed`, `g_ending_armed`, `g_quit_pending` (0/1/2),
`g_resume_load`, `g_waited` (shared), set from five functions. Now: one
`SaState g_state`, one transition function `sa_go(to, why)` with an allowed-
transition table. A refused transition is counted (`sa_illegal_transitions`)
and logged as `[AUTOSAVE] state X -> Y refused (why)`. `PC_SA_LOG=1` logs
every change.

| From | Event | To |
|---|---|---|
| IDLE | first VSync with `TOKI_RESUME=1` | RESUME_LOAD |
| RESUME_LOAD | boundary (load `resume`, or "nothing to resume") | IDLE |
| IDLE | VSync: planner 0x34, in a game, autosave on, period key (`[saves] autosave_every`: the month, or `pass_days`) ≠ the last one | WEEK_ARMED |
| WEEK_ARMED | boundary in 0x34/1, machine quiet, save taken or failed | IDLE (week recorded) |
| WEEK_ARMED | boundary: planner left / autosave off / out of game | IDLE (week recorded as skipped) |
| IDLE, WEEK_ARMED | VSync: step becomes 99 from another step, year 98, ending save on | ENDING_ARMED (from WEEK_ARMED the week is recorded as skipped) |
| ENDING_ARMED | boundary, machine quiet, save taken or failed | IDLE |
| ENDING_ARMED | left step 99, or ARM_LIMIT (1800) **game frames** busy | IDLE |
| WEEK_ARMED, ENDING_ARMED | any restore (`sa_after_load`) | IDLE |
| IDLE, RESUME_LOAD, WEEK_ARMED, ENDING_ARMED | quit asked in a game, resume on (`sa_defer_quit` → 1) | QUIT_PENDING |
| QUIT_PENDING | boundary: `resume` written (or failed: deleted) | QUITTING (`hal_quit_now`) |
| QUIT_PENDING | boundary: no game in progress any more | QUITTING |
| QUIT_PENDING | VSync: `PC_QUIT_WAIT_MS` (4000) without a boundary | QUITTING; the older `resume` is deleted |
| QUIT_PENDING, QUITTING | quit asked again | (unchanged; `sa_defer_quit` → 0: quit at once) |

Invariants: one save in flight at a time (`qs_writer_wait` before picking a
slot); a quit supersedes an armed save; a load never cancels a quit; the
`resume` entry on disk is never older than the last quit of a session that
played. Data, not state: `g_last_week`, `g_seen_game`,
`g_ending_prev_step` (the edge detector).

Tests: `pc/tests/save_auto_harness.c` (stubs; `SaveAutoMachine`: week,
week left, load disarms, ending wait counted per frame, quit, quit timeout,
no refused transition; four mutations caught), `test_saves.py` (port),
`QuitDuringAGame` (port).

### 3.2 Quicksave requests and writer — `pc/hal/quicksave.c`

Kept. Threads: game, plus one writer pthread per save (joined).

| Machine | Variables | States |
|---|---|---|
| requests | `g_req_save[]`, `g_req_load[]` (empty = none), `g_busy_tries` | idle / save pending / load pending / both (the load wins and drops the save) / save retrying (up to 600 boundaries while busy) |
| writer | `g_worker_started`, `g_job_done` (atomic, release/acquire), `g_job_reported` | idle / running / done-unreported / reported |

Transitions: hotkeys, the browser and the toolkit (`qs_request_*`) set a
request; `qs_boundary` acts on it at the next boundary (it runs from
`boundary_body` before the rewind, and again inside the toolkit's command
loop). The writer is joined by `qs_writer_wait` before a load, a listing, a
delete, a new id, and at shutdown.

Invariants: a load supersedes a pending save, and a second request
replaces the first -- both said with a note (`Save to slot 3 cancelled: a
load came first`, `Saving slot 4 instead of slot 3`; R3, fixed); no request
is carried out while the memory card has a transfer in flight
(`memcard_busy`, as the autosave; at most `QS_CARD_LIMIT` calls; R7,
fixed); `slot0` (undo) is written before every load of another entry. The
synchronisation is correct.

Tests: `test_quicksave.py`, `test_saves.py`, `test_save_browser.c`,
`test_ui_state_machines.SaveRequestsAreReported`, `QuicksaveWaitsForTheCard`.

### 3.3 Rewind — `pc/hal/rewind.c`

Kept. Threads: game, plus the coder thread (`g_job` under `g_mu`, `g_cv`).

States: OFF (`rw_enabled()` false) / FAILED (`g_init` −1) / EMPTY (`g_init`
1, no head) / READY (head) / CODING (`g_job` 1) / REWINDING (inside
`interactive()`, which holds the boundary). `was_on` (static in
`rw_boundary`) detects the off edge (frees the buffers).

| From | Event | To |
|---|---|---|
| OFF | boundary with rewind on | `setup` → EMPTY, or FAILED |
| EMPTY | boundary | READY (first snapshot = head) |
| READY | boundary at the interval, coder idle | CODING (spare filled, worker signalled) |
| CODING | worker done | READY |
| READY | key held / `TOKI_REWIND_HOLD` | REWINDING |
| REWINDING | key released, steps done, close button, quit | READY (restore, `dbg_state_after_restore(REWIND)`) |
| any | `rw_after_load` (a file load) | EMPTY |
| any on, FAILED | turned off in the menu | OFF (memory freed; a failed set-up is tried again when turned on, R13) |
| EMPTY, READY | boundary while `memcard_busy()` | unchanged (no snapshot, no rewind started; counted busy, R7) |

Invariants: no snapshot while the coder is busy (skipped, counted); a
restore never touches the live memory card; the keys held for the rewind
reach the game only once released.

Fixed: the close button during a rewind was only peeked at (bug **B2**);
R7 and R13 (section 4). Tests: `test_rewind.py`, `test_rewind_codec.c`,
`QuitDuringAGame`, `EveryRestoreTellsEveryObserver`,
`test_ui_state_machines.RewindMachine` (`rewind_harness.c`).

### 3.4 Presenter hold, the pause loop — `pc/hal/present.c` (DOCUMENTED)

Kept. Thread: game (the pause loop runs inside VSync, deep in game code).

The effective state is the predicate `held_here()`:
`!sa_quit_pending() && (g_user_paused || g_focus_paused || pc_menu_is_open()
|| backlog_is_open())`. So the "states" are RUNNING, USER_PAUSED,
FOCUS_PAUSED, MENU, BACKLOG, any combination of them, and QUIT_DEFERRED,
which overrides all holds.

| Event | Site | Effect |
|---|---|---|
| pause hotkey | `run_hotkey` | toggles `g_user_paused` |
| focus lost / gained | `pc_present_handle_event` | sets / clears `g_focus_paused` (not in automated runs) |
| menu hotkey, controller menu button, Ctrl+S, Ctrl+I, the resume prompt at launch | `menu_open`, `pc_menu_open_page` | MENU (refused while the backlog is open) |
| Ctrl+B, the wheel, the backlog button | `backlog_toggle` | BACKLOG (refused while the menu is open) |
| quit asked with resume on | `sa_defer_quit` | QUIT_DEFERRED: the loop lets go so the game reaches the boundary |
| loop entry | start of `pc_present_pause_loop` | `pad_hold_changed(1)`: relative grab released, OS cursor shown (A6) |
| loop exit | end of `pc_present_pause_loop` | menu closed (settings saved), backlog closed, input hold-off armed (A2), `pad_hold_changed(0)`, deferred-hotkey note cleared, mixer un-paused |
| quicksave / quickload hotkey while held | `run_hotkey` | the request waits for the boundary; "Quicksave when you resume" on the pause label, the menu footer or over the backlog (A14) |

`hal_frame_begin` stops draining events when a hold starts, so the pause
loop gets the events queued behind it.

Invariants: one overlay at a time; a key that ended or worked a hold
never reaches the game (the hold-off at every hold end, released only
when every bound key is up); a mouse button let go during a hold is let go
for the game; the pointer is the player's while held; audio paused exactly
while held; a drain that opens the menu stops there (`hal_poll_quit` as
`hal_frame_begin`), and shutdown closes an open menu (its changes saved).

Fixed: Ctrl+S opened the menu over the open backlog (bug **A4**); A2, A3,
A6, A11, A14 (section 4). Tests: `test_settings_menu.py`,
`test_presentation.py`, `OneOverlayAtATime`,
`test_ui_state_machines.HoldEndsCleanly`, `SettingsAndQuit`.

### 3.5 Settings menu model — `pc/hal/menu.c` (DOCUMENTED)

Kept. `g_open`, `g_page`, `g_sel`, `g_stack[]` / `g_stack_sel[]` /
`g_depth`, `g_dirty[]` / `g_ndirty`. States: CLOSED, then OPEN at depth
0..N on a list page or a custom page. Transitions: `pc_menu_open`,
`pc_menu_open_page`, `pc_menu_input` (BACK at depth 0 closes), and
`pc_menu_close` (saves the dirty keys; −2 = refused by a page). Custom pages
(the save browser) may close the menu from inside their input callback.
Invariant: the dirty keys are written exactly once, at close -- also when
the run ends with the menu open (`pc_present_shutdown` closes it; A11,
fixed). Hotkey changes are not dirty keys: session-only, as PLAYER-GUIDE.md
documents (A9, decided). Tests: `test_settings_menu.c`,
`test_settings_menu.py`, `test_ui_state_machines.SettingsAndQuit`.

### 3.6 Input hold-off, mouse buttons, device swap, cursor grab — `pc/hal/pad_device.c`, `pc/hal/pad.c`

| Machine | Variables | Kept? | States and transitions |
|---|---|---|---|
| hold-off | `pad_device.c:g_hold_off` | kept (since 2026-11-03) | armed by `pad_suppress_until_release` (every end of the presenter's hold, menu / backlog close, rewind end; re-armed every poll while the menu is open) → disarmed at the first poll with no bound key or button down -- counted without the Ctrl/Alt/GUI chord filter, so a key a held Ctrl hides still holds it (A2). Keyboard and controller only; the mouse is event-driven: a release during a hold is always passed on (A3) |
| mouse buttons and assist | `g_mouse_left/right` (kept), `g_mouse_hide/add` (restored, the quick-text filter), `pad.c:g_assist_mouse` (kept) | | SDL events, the text-assist pulse, `pad_device_release` on focus loss. The assist and the physical left button share one bit (risk R9) |
| port-1 device | `g_attached`, `g_gap_left`, `g_last_*_tick` | restored (device model) | an explicit model with tests (`test_pad_mouse.c`) |
| cursor grab | `pad.c:g_focus`, `g_grabbed`, `g_cursor_hidden`, `g_held`, `g_ptr_*`, `g_steering`, `g_mouse_mode` | kept | recomputed in `pad_update_buffers` (every pad poll), on focus loss and by `pad_hold_changed` at the pause loop's entry (released, cursor shown) and exit (A6) |

Tests: `test_pad_mouse.c` (`test_pad_hold_off`), `test_pad_record_binding.py`,
`test_ui_state_machines.HoldEndsCleanly`.

### 3.7 Quick text — `pc/hal/quick_text.c` (explicit already)

Restored with the machine. `g_mode` is an enum (`QT_PASS`, `QT_HOLD`,
`QT_CUT`), set once per press at the poll from `decide()`; `g_pending` (−1 =
none) and `g_pending_loaded` carry a completion to the frame end, applied
only if the page is still the same. No change needed. Tests:
`quick_text_harness.c` (with five source mutations), `test_quick_text.py`.

### 3.8 Auto-advance, skip read pages, skip intro, fast-forward — `pc/hal/enhance.c`

Kept. Game thread, except `g_ff_active` (volatile; read by the audio
thread).

| Machine | Variables | States |
|---|---|---|
| auto-advance / skip-seen | `g_wait`, `g_press` (polls left of an auto press), `g_last`, `g_arrow_age` (`NEVER` sentinel), `g_arrow_screen` | IDLE → WAITING (arrow seen on this screen, page complete) → PRESSING (2 polls) → IDLE; any player press → IDLE |
| skip intro | `g_intro` (−1 undecided / 1 on / 0 done), `g_tap_from` (−1 none), `g_tap_next` | UNDECIDED → TAPPING ↔ GAP → DONE |
| fast-forward | `g_ff_active`, plus `system.c:g_ff_skipped` | OFF / ON (key held or `TOKI_FF_HOLD`) |

Fixed: an auto press in flight, or a wait half counted, carried across a
restore into the loaded page; it now starts over (`text_assist_after_load`,
part of **S2**). Open: while quick text holds a press back (`QT_HOLD`), the
auto-advance sees no player press (R8). `g_ff_skipped` is not reset when
fast-forward ends (one shown frame may be skipped late; cosmetic). Tests:
`test_enhanced_profile.py`, `test_skip_intro.py`, `test_skip_seen.py`,
`test_frame_pacing.py`.

### 3.9 Backlog capture, view and voice replay — `backlog_capture.c`, `backlog.c`, `backlog_view.c`, `voice_replay.c`

| Machine | Variables | Kept? | States |
|---|---|---|---|
| capture | `g_cand` (0 none / 1 built / 2 committed), `g_cand_key`, `g_waiting`, `g_arrow_age`, `g_stream.open/serial`, `g_attached_serial` | restored | edge detection on the text engine at each frame end; the voice stream opens on the first XA sector of a page |
| view | `g_open`, `g_sel` (−1 none), `g_view_replay` | kept | CLOSED ↔ OPEN; a replay started in the view stops when it closes |
| voice replay | `g_pcm`, `g_frames`, `g_pos`, `g_active` (volatile) | kept | IDLE ↔ PLAYING; written by the game thread under the audio lock, read by the mixer |

Invariants: the game's own voice line stops a replay (`backlog_game_voice`);
the view never takes input while the menu is open. Risks: R4 (the list and
the replay survive a load into another save, by design), R10 (`g_stream.open`
not reset while the backlog is off). Tests: `backlog_harness.c`,
`test_backlog.py`.

### 3.10 Seen text — `pc/hal/seen_text.c`

Kept. `g_loaded` (0 / 1 / −1 unusable) plus once-only `on` / `env`. The file
is appended with 8-byte records (O_APPEND). Risk R11: a torn record after a
crash misaligns every later record. Tests: `test_skip_seen.py`.

### 3.11 Extras — `extras.c`, `extras_gallery.c`, `extras_state.c`

Kept. Game thread (plus the file writer for pictures).

| Machine | Variables | States |
|---|---|---|
| ending detector | `g_seen_init`, `g_end_armed` | UNINIT → ARMED (outside step 99, or at 99/0/0) → DISARMED (decision recorded or passed) |
| gallery still | `g_still.st` (enum `ST_NONE`, `ST_COLLECT`, `ST_SHOWN_WAIT`) | explicit |
| gallery event | `g_ev.on/frames/idle/unseen` | implicit: watching an event scene's sheets for `EVENT_WINDOW` frames |
| disc-read ring | `g_loads[]`, `g_gen` | names pictures by the file read last at a VRAM area |

The hook switch `g_extras_gpu` (in `gpu.c`) is now kept, and
`extras_after_load` re-decides it after every restore, including a rewind
(before: a rewind left the snapshot's value and never re-armed the ending
detector, **S2**). Tests: `extras_harness.c`, `test_extras.py`.

### 3.12 Writers and the watchdog — `card_writer.c`, `async_io.c`, `watchdog.c`

| Machine | Variables | States | Notes |
|---|---|---|---|
| card writer | `g_started`, `g_start_failed`, `g_have_pending`, `g_busy` | NOT_STARTED / FAILED (inline writes) / IDLE / PENDING / WRITING | kept; correct; joined by `card_writer_wait` at shutdown and before a trace exit |
| file writer | `g_mode` (−1 unknown / 0 inline / 1 thread), `g_lock`, `g_wake`, `g_idle`, `g_thread`, `g_head/g_tail`, `g_queued`, `g_busy` | UNKNOWN → INLINE or THREADED; queue empty / queued / running | **kept since 2026-11-03** (S1); flushed at exit (`atexit`) and by `ex_picture_read` |
| watchdog | `g_armed` (+ `g_deadline` on Windows) | DISARMED ↔ ARMED around `game_main` | the pause loop feeds it; a blocking wait inside a menu callback does not (R5) |

### 3.13 First-run setup and the verify job — `setup_screen.c`, `asset_setup.c` (DOCUMENTED)

`g_s.state` is an explicit enum (`ST_FIND` .. `ST_DONE`). Implicit around
it: `quit`, `done_at`, `g_drawn_since_change`, `g_ctl.cancel` (volatile, read
by the worker), the atomic `g_job_done`, `g_thread`; on disk, the
`.assets.prepare.lock` directory and the `.assets.stage-PID-*` folders. The
verify job (`g_vthread`, atomic `g_vdone`, `g_vrc`, −1 = never run) is
joined lazily from `verify_note`, i.e. only when its row is drawn.

Esc and the close button on "the game data is ready" (ST_DONE) leave: the
setup returns NULL and the program exits; the data is kept, so the next
launch starts the game (A5, fixed; Esc closes here as on every other setup
screen -- the screen moves on by itself after 1.2 s, so "stay" would mean
nothing). `TOKI_SETUP_SCRIPT` is read on that screen too. Risks: R12
(leftovers after a kill; the verify thread not joined if its row is never
redrawn). Test: `test_ui_state_machines.SetupReadyScreen`.

### 3.14 Save browser — `save_browser.c` (explicit already)

`g_b.popup` is an enum (NONE / ACTIONS / CONFIRM / MESSAGE), with `psel`,
`sel`, `top` and the thumbnail cache, all reset in `enter()`. Loads and
saves are requests acted on at the next boundary. Tests:
`test_save_browser.c`, `test_saves.py`.

### 3.15 Status page, language, profile, toast and OSD

| Machine | Owner | Notes |
|---|---|---|
| status page | `status_view.c` | `status_enabled()` latches once; `g_page`, `g_tab`, `g_sel`, `g_top`. Kept since 2026-11-03 (S1) |
| language | `lang.c` | decided once in `hal_lang_init`; the menu writes only the setting (applies on restart). Restored with the machine (2.1) |
| profile | `profile.c` | latched at the first call; kept |
| toast / OSD / notes | `present.c`, `present_layout.c` | toast and OSD: one slot each, mutually clearing, wall-clock timers, for results and live lines (slot list, rewind). Notes (`pc_present_note`, A8 fixed): a queue of 8, each timed only while it is on screen; priority menu > PAUSED > OSD > toast > note. Toasts (and the extras' unlock popups) show only with `[system] toasts = true` (off by default; automated runs: `TOKI_TOASTS=1`); notes always show. Failures (screenshot, settings not saved, quicksave failed or refused) are notes. The launch's "cannot be resumed" and "English unavailable" and the saves' cancelled-request messages are notes |

### 3.16 Audio: the SPU register queue and the device hold — `pc/hal/spu.c` (DOCUMENTED)

| Machine | Variables | States |
|---|---|---|
| register queue | `g_rt{q, head, count, pub_valid, play, play_valid, extra, resync_floor, last_tick}`, `rt_queue_on()` | DIRECT (parity mixer, or queue off) / QUEUED (writes stamped with emulated time, replayed by the callback at their sample) / RESYNC (clock jumped: flush) |
| device hold | `g_held`, the SDL pause state | RUN / HELD (`spu_hal_set_paused`) / HELD + side audio (voice replay) / CLOSED (FMV) |

Push and publish run on the game thread, apply on the audio thread, all
under the device lock. `g_rt` is kept, so after a restore the queue still
held the old time line's writes (C1, section 5): since 2026-11-03
`spu_hal_after_restore` drops them. While fast-forward is held the writes
go straight to the core (C6). `g_held` is kept since 2026-11-03.

### 3.17 Audio: the music pack observer — `pc/hal/music_pack.c` (DOCUMENTED)

Per access `Acc{open, handle, flags, vol}`; `g_cur`, `g_split_*`,
`g_core_*`, `g_engaged`, `g_cur_pack`, `g_dump_*`. Per stream
`{paused, stopping, may_loop, finished, done, env}`. States: CLOSED → OPEN →
PLAY ↔ PAUSE → STOP or ENDED → CLOSE, driven by edges on libsnd's score
flags each VBlank; the stream PLAY → PAUSED / STOPPING → DONE (envelope on
the audio thread). Kept. Since 2026-11-03: `music_pack_after_load` and a
re-identification on PLAY edges (C4); the file is decoded ahead into a
ring on the game thread (`stream_fill`; `stream_replan` when the song ends
by itself) and the dump goes through a ring the game thread writes
(`dump_drain`), so the audio thread does no file I/O and no log line is
written under the lock (C5).

### 3.18 CD and XA — `pc/hal/cd.c`, `cd_audio.c`

`cd.c`'s drive is an explicit enum (`g_drive`) with coherent command and
transfer flags; no change. `cd_audio.c`: the XA FIFO (`g_fifo_head/count`),
`g_cur_set`, `g_muted`, `g_have_window/g_window_end`, and `priming` /
`primed` (function statics): DRY / PRIMING / PLAYING on the mixer side.
Pushes and resets run on the game thread under the lock. Proposals C6, C8.

### 3.19 Other device and GPU-side machines

| Machine | Owner | Assessment |
|---|---|---|
| SPU DMA | `spu_transfer.c` | explicit `stage` 0/1/2 plus a request serial; clean |
| VBlank IRQ | `irq.c`, `sdk_irq.c` | `cpu_enabled`, `delivering`, `in_edge`, `pending`; clean |
| GPU queue | `gpu_queue.c` | `active`, `notifying`, `deferred`, `generation`; clean |
| overlay | `overlay.c` | `g_loaded`; restored with the machine |
| hi-res shadow | `gpu_hires.c`, `gpu.c` | kept and resynchronised after a restore since 2026-11-03 |
| texture dump / pack | `gpu_texrep.c` | kept since 2026-11-03; ownership re-validated per texel against VRAM (`g_oval`) |
| movie | `libpress.c` | synchronous; open streams are QUIESCE rules (no save while open) |
| legacy FMV player | (deleted) | `str_player.c` had no caller and an unsafe audio stream; deleted 2026-11-03 with `spu_hal_pause_for_fmv` / `resume_after_fmv` (C14) |
| frame pacing | `system.c` | `g_pace_resync`, `scheduled`, `g_pace_deadline`; resynchronised by every restore since 2026-11-03 |

---

## 4. Bugs found and fixed

Each fix has a test that fails before the fix (the negative control) and
passes after it. "Pre-fix build" means the binary of `ef5bd6be04`, run with
`STATE_MACHINES_BINARY=<it>`.

| Id | Bug | Fix | Test | Negative control |
|---|---|---|---|---|
| **S1** | Host resources were saved in states and written back by a load: the file writer's mutex/threads, the texture dumper's heap tables and `FILE*`, the hi-res shadow's buffers and textures, the input configuration, the status latch, the held mouse buttons and hold-off. A state saved under enhanced + 2× + texture dump and loaded in another process **hung in `pthread_mutex_lock`, then crash-looped** | KEEP rules for `async_io.c`, `gpu_texrep.c`, `pad.c`, `status_view.c`, `g_hires`, `gpu.c:hires_tex/_w/_h`, `gpu_hires.c:pixels/_scale`, `g_texrep_on`, `g_texrep_replace`, `g_extras_gpu`, `pad_device.c:g_mouse_left/right/g_hold_off`, `spu.c:g_held` (`dbg_state.c`); shadow resync after a restore | `HostResourcesNotInStates` (audit clean; another process loads it and runs 60 frames) | pre-fix build: the audit lists 13 host objects; the load exits −6 after the crash loop |
| **S2** | The restore paths told different observers: the toolkit's `load` / F9 / `PC_LOADSTATE` skipped the pacing resync and the autosave; a rewind skipped the extras (ending detector, gallery hooks); an overlay mismatch skipped `qs_load_now`'s hooks | one list, `dbg_state_after_restore(how)`, called by `dbg_state_load` (before its overlay check) and by the rewind; the per-path calls removed (`quicksave.c`, `rewind.c`, `dbg_cmds.c`); `text_assist_after_load` added | `EveryRestoreTellsEveryObserver` (four paths) | pre-fix build: no path prints its observers (all four fail) |
| **S3** | Consequence of S2: the toolkit's `load` of a later moment in a paced session slept the whole emulated gap (38 s) and the watchdog fired | (S2) | `PacedToolkitLoad` | pre-fix build: 7 `[CRASH]`, ~50 s |
| **B2** | The close button during an interactive rewind was only *peeked at* (`SDL_PEEKEVENT`): the deferred quit's resume save found the writer busy, the next frame's event poll delivered the same SDL_QUIT again as a second request, which quits at once — **no resume state** | `SDL_GETEVENT` (`rewind.c`) | `QuitDuringAGame.test_close_during_rewind_writes_resume` (`TOKI_REWIND_HOLD=N:3:quit`, writer busy through `PC_QS_WRITE_DELAY_MS`) | a build with `SDL_PEEKEVENT` restored: no `[RESUME] resume state written` |
| **B4** | The quit timeout (no boundary within 4 s) quit without deleting the previous session's `resume`, which the next launch offered as "where you left off" | delete it in the timeout branch (`save_auto.c`) | `SaveAutoMachine.test_quit_timeout` | mutation "the quit timeout keeps the older resume" |
| **F3** | `sa_boundary` runs twice per boundary (`boundary_body` calls `qs_boundary` before the rewind and again in the command loop), so the ending save's ARM_LIMIT (1800) counted calls, not frames: it gave up after 900 busy frames | the wait is counted once per game frame (`waited_one`) | `SaveAutoMachine.test_machine` (`ending_wait_counts_frames`: 1300 busy frames, then saved) | mutation "the busy wait counted per call" |
| **A7** | After the resume state was written at the boundary, the loop ran one more whole game frame before ending (its card write was not in the resume state) | `main.c`: leave the loop right after the boundary when the run is over | `QuitDuringAGame.test_no_frame_after_the_resume_state` (`PC_MARK_EVERY=1`) | pre-fix build: a `[MARK]` after the resume line |
| **A4** | Ctrl+S (save browser) over the open backlog opened the menu too (drawn over the backlog, keys going to the invisible backlog) | the same `bl_open()` guard as MENU and STATUS (`present.c`, one line) | `OneOverlayAtATime` | pre-fix build: `settings menu open (saves)` logged |
| **R-SA** | (refactor) `save_auto.c`'s six flags → one enum and one transition function with refused-transition logging; WEEK_ARMED → ENDING_ARMED made explicit (the old flags allowed both armed at once) | — | `SaveAutoMachine` (+ two rule mutations: a load must drop an armed save; the planner must arm only from IDLE) | the mutations |

Second round (2026-11-03, the UI and input items of section 5 and the
risks of section 6). "Hooks-only build" is `e7e60f76d4` (the test hooks of
`pc/hal/pad.c`, `dbg_cmds.c`, `setup_screen.c`, `quicksave.c`, plus the
one-line `pad_script_key_taken` call in `present.c`), run with
`STATE_MACHINES_BINARY=<it>`; every port test of
`pc/tools/test_ui_state_machines.py` fails on it (12 of 12). The C
harnesses carry their own source mutations.

| Id | Bug | Fix | Test | Negative control |
|---|---|---|---|---|
| **A2** | A hold ended by Ctrl+Space with Ctrl let go before Space handed Space's circle to the game. Two holes: no hold-off at a pause's end, and the hold-off ended at the first poll where no game button counted -- with Ctrl down, Space does not count, so it ended while Space was still down | `pad_suppress_until_release()` at every end of `pc_present_pause_loop`; the hold-off is released by the keys DOWN, not the buttons counted after the chord filter (`pad.c`) | `HoldEndsCleanly.test_pause_key_released_out_of_order` (`keydown` / `keyup` script lines; at the title, `pad_data` must not show circle until Space is released; a later press does) | hooks-only build: `pad_data=0x20` at ticks 2410 and 2420; with only the first fix, still 0x20 (measured) |
| **A3** | A mouse button let go during a hold stayed down for the game (the pause loop swallowed `SDL_MOUSEBUTTONUP`; so did `dispatch_event` over the menu / backlog) | the release always reaches `pad_handle_event` (`present.c`, `system.c`) | `HoldEndsCleanly.test_mouse_release_during_pause` (`mbutton` script lines; `dump` prints `mouse=LR`) | hooks-only build: `mouse=10` at ticks 410 and 450 |
| **A5** | Esc or the close button on "the game data is ready" started the game | `pc_setup_run` returns NULL when the loop ended by a quit in ST_DONE; data kept (decided: Esc leaves too) | `SetupReadyScreen.test_escape_on_ready_leaves` (and the next launch starts the game), `test_close_on_ready_leaves` | hooks-only build: the game ran its 30 frames after the setup (`PC_TRACE_FRAMES=30 reached`) |
| **A6** | The pause and the menu kept the relative-mode grab and the hidden cursor | `pad_hold_changed(held)` at the pause loop's entry and exit (`pad.c`, `present.c`) | `HoldEndsCleanly.test_pause_releases_the_grab` (`TOKI_MOUSE_MODE=relative`, `PC_DEBUG=pad`) | hooks-only build: no `cursor released` between `paused` and `resumed` |
| **A8** | One OSD slot: a second message replaced the first ("cannot be resumed" by "English unavailable"), and one shown under the menu expired unseen | notes: a queue (`present_notes_*`, SDL-free in `present_layout.c`) timed only while on screen; `pc_present_note` for the two launch messages and the saves' cancelled requests | `PresentNotes` (`pc/tests/present_notes_harness.c`: two_in_order, covered_waits, queued_while_covered, duplicates_and_capacity) | mutations "a new note replaces the one shown" (two_in_order fails), "the time runs while covered" (covered_waits fails) |
| **A9** | Hotkey changes (fullscreen, filter, scale, scanlines, vsync, volume) are not written to settings.ini | decided: intended -- PLAYER-GUIDE.md says they "last until you quit"; said in `run_hotkey`; the "settings not saved" line lists its keys | `SettingsAndQuit.test_hotkey_changes_are_session_only` (Ctrl+- then a menu change: only `display.scale` is listed) | the menu's own change is listed (the listing would show a hotkey key) |
| **A11** | Settings changed in the same event drain as a quit were lost: `hal_poll_quit` drained past the menu-opening key, and shutdown left the menu open | `hal_poll_quit` stops at an event that holds the game; `pc_present_shutdown` closes an open menu (saving) and the backlog | `SettingsAndQuit.test_menu_change_and_close_in_one_drain` (toolkit `keys`, `winclose`, `pause`) | hooks-only build: `settings menu open`, then `[HAL] Shutdown` -- no close, nothing saved |
| **A14** | F5 / F9 while paused (or in the menu / backlog) was deferred to the resume silently (its toast expired under the pause label) | "Quicksave when you resume" on the pause label, the menu footer, over the backlog | `HoldEndsCleanly.test_quicksave_while_paused_is_said` (`TOKI_QS_HOTKEYS=1`, `PC_PRESENT_HASH`: the paused frames are not all one picture; the save is made after the resume) | hooks-only build: one paused picture, no note |
| **R1** | `KEEP_FILES` was opt-in: a new pc/hal module's data was machine state by default, unseen | `MACHINE_FILES` in `dbg_state.c` lists the pc/hal files a state saves on purpose; the inventory dump prints each record's file and the list. Found and moved to KEEP: `present_crt.c` (1.2 MB CRT scratch in every state and rewind snapshot), `asset_map.c`, `sweep_shot.c`, `text_audit.c`; 2026-11-06: the CONTROLS page `controls_view.c` (the key legend and its one-time hint flag) and the GUIDE files `guide_view.c`, `guide_model.c` (a heap document), `guide_rules.c`, `guide_rules_dates.c` (disc-table caches) | `StateOwnershipIsListed.test_every_pc_module_in_a_state_is_listed` | `test_negative_controls`: `gpu.c` taken off the list, and a new module's object, are both reported |
| **R3** | A load superseding a pending save, or a second save / load request replacing the first, said nothing | notes: `Save to slot 3 cancelled: a load came first`, `Saving slot 3 instead of slot 2`, `Loading ... instead of ...` (`quicksave.c`) | `SaveRequestsAreReported.test_second_save_and_load_over_a_save` (toolkit `qsave 2`, `qsave 3`, `qload 1` at one boundary) | hooks-only build: no message |
| **R7** | Verified: a quicksave requested at a boundary with a card request in flight wrote a state whose `memcard.c:g_requests` held operation 8 -- its load would finish that old request onto the newer live card (a load keeps the live card); a load would abandon a live transfer half-written. The rewind's snapshots and restores likewise | requests wait while `memcard_busy()` (`QS_CARD_LIMIT` calls at most), as the autosave; the rewind takes no snapshot and starts no rewind then (`rewind.c`) | `QuicksaveWaitsForTheCard.test_quicksave_waits_for_the_memory_card` (new_game's system-file creation, toolkit `when memcard.c:g_requests:4!=0 do qsave 3`: the state holds no transfer); `RewindMachine` card | hooks-only build: the state holds operation 8 in flight; mutation "snapshots ignore the memory card" |
| **R13** | The rewind stayed off for the session after a failed set-up (`g_init` −1 never retried); `TOKI_REWIND_HOLD` fired with nothing kept left `g_sim_active` set, turning the player's first real rewind into the test hold (with its close-button press) | the off edge resets a failed set-up; the early return spends the hold (`rewind.c`) | `RewindMachine` retry, sim (`pc/tests/rewind_harness.c`) | mutations "a failed set-up stays failed", "a spent test hold stays active" |

Behaviour: none of these changes what the game sees in the three
playthroughs (parity and enhanced trace, frame and audio hashes compared
before/after: section 7).

---

## 5. Proposed fixes in the audio and UI files

### Audio (`pc/hal/spu.c`, `music_pack.c`, `cd_audio.c`)

Status 2026-11-03 (tests in `test_audio_realtime` CadenceTests,
`test_music_pack` HarnessTests, `test_cd_audio`, each with a negative
control): **C1 fixed** (`spu_hal_after_restore`, observer 7), **C4 fixed**
(`music_pack_after_load`, observer 8, and the PLAY-edge id check), **C5
fixed** (decode-ahead and dump rings; a stdio spy sees 0 calls in
`music_pack_mix`), **C6 fixed** (no queue while fast-forward is held; the
XA FIFO trimmed to its margin on the release), **C7 confirmed and fixed**
(the health line now counts key-ons a flush's key-off cancelled: 1 in the
real-time parity title -> name entry; a bank upload no longer flushes the
queue -- a queued write is a register write, the RAM is the new data for
it either way), **C8 fixed** (a
real-time Pause keeps the margin's tail), **C12 fixed** (`spu_reset_write`
flushes the queue first), **C14 done** (`str_player.c` deleted). C9 open.

* **C1 (confirmed, high)** — the real-time register queue (`g_rt`, kept)
  survives a quickload or rewind; the next callback replays the old time
  line's key-ons and voice writes into the restored SPU (ghost or stuck
  notes). Fix: `void spu_hal_after_restore(void)` in spu.c, under the device
  lock: `g_rt.count = g_rt.head = 0; g_rt.pub_valid = g_rt.play_valid = 0;
  g_rt.extra = 0;` (drop, do not flush), then add it to
  `dbg_state_after_restore` (one line; the hook list has the slot).
* **C4 (confirmed, medium)** — the music pack observer detects a song
  change only by handle (the SEQ address), which a SEQ in another bank can
  share; across a load (kept observer, restored libsnd) a different song at
  the same handle keeps the old replacement playing. Fix:
  `music_pack_after_load()` (stop `g_cur`, close the dump, `g_acc[i].open =
  0`), called from `dbg_state_after_restore`; on a PLAY edge, compare the
  track id as well as the handle.
* **C5 (confirmed, medium)** — file I/O on the audio thread (`fwrite` of the
  dump in `music_pack_mix`, decoding in `stream_mix`) and under the audio
  lock (`event()` log, `dump_close_locked`). Fix: a ring drained in
  `music_pack_tick`; decode ahead on the game thread; log after unlocking.
* **C6** — fast-forward: the register queue stays on (it can overflow its
  16,384 entries and burst on release) and the XA FIFO fills to its cap
  (voice lags up to 0.5 s after release). Fix: `rt_queue_on()` false while
  `hal_ff_active()`; trim the FIFO to the margin on the release edge.
* **C7 (suspected)** — the queue is flushed (merged) at every SPU DMA commit
  and `spu_reset_read`: a key-off can cancel a pending key-on there.
* **C8 (suspected)** — real-time `cd_audio_reset_decoder` keeps 0 frames:
  the decoded tail of a voice line (≥ 70 ms) is cut at every pause/seek.
* **C9 (developer path)** — `cd_audio.c` `g_have_window` from a parity-mixer
  state loaded into a real-time session: reset it after a restore when not
  in parity mode.
* **C12 (suspected)** — `spu_reset_write` writes voice/key registers without
  flushing the queue first.
* **C14** — `str_player.c` is dead and its `SDL_AudioStreamPut` races the
  callback: delete it with `spu_hal_pause_for_fmv` / `resume_after_fmv`.

### PC UI (`pc/hal/present.c`, `menu.c`, `setup_screen.c`)

All eight were fixed or decided on 2026-11-03 (section 4, second round;
`pc/tools/test_ui_state_machines.py`). The proposals as written:

* **A2 (confirmed)** -- FIXED 2026-11-03 (section 4; the hold-off also had to count keys a held Ctrl hides) — Ctrl+Space (pause) unpaused with Ctrl released
  before Space hands CIRCLE to the game (same for Alt+Tab focus regain).
  Fix: `pad_suppress_until_release()` at the end of `pc_present_pause_loop`
  (every hold end). Test needs a key-down/key-up script verb (today `key`
  queues both at once).
* **A3 (confirmed)** -- FIXED 2026-11-03 (section 4) — a mouse button released during a hold stays down for
  the game (`SDL_MOUSEBUTTONUP` is swallowed by the pause loop; the hold-off
  does not cover the mouse). Fix: always forward `SDL_MOUSEBUTTONUP` to
  `pad_handle_event`, or `pad_device_release()` on hold end.
* **A5 (confirmed)** -- FIXED 2026-11-03 (section 4; Esc leaves too) — the setup screen: Esc or the close button on "game
  data is ready" (ST_DONE) still starts the game. Fix: `if (quit) return
  NULL;` after the loop (or re-push SDL_QUIT). Test needs `TOKI_SETUP_SCRIPT`
  to accept `quit` in ST_DONE.
* **A6** -- FIXED 2026-11-03 (section 4) — pause and menu do not release the relative-mode grab or show the
  cursor (only `pad_update_buffers` recomputes it). Fix: a
  `pad_hold_changed(held)` called at pause-loop entry and exit.
* **A8** -- FIXED 2026-11-03 (section 4: notes) — one OSD slot: `hal_lang_boot_fixup` overwrites `sa_launch`'s
  8-second "cannot be resumed" note; an OSD shown under the menu expires
  unseen. Fix: a small queue, timers paused while overlaid.
* **A9** -- DECIDED 2026-11-03: intended, documented (section 4) — hotkey changes (volume, filter, scale, scanlines, vsync,
  fullscreen) are session-only by accident (no `mark_dirty`): mark them or
  document them as session-only.
* **A11 (suspected; confirmed)** -- FIXED 2026-11-03 (section 4) — `hal_poll_quit` drains without the holding break and
  `hal_shutdown` does not close the menu: settings changed in the same
  drain as a quit are lost. Fix: stop the drain on `pc_present_holding()`;
  `pc_menu_close()` in `hal_shutdown`.
* **A14** -- FIXED 2026-11-03 (section 4: a note on the pause label / menu footer) — quicksave / quickload hotkeys pressed while paused or in the
  menu are silently deferred (their toast is hidden). Fix: a toast that
  says so, or refuse while held.

---

## 6. Remaining risks

* **R1 — the KEEP lists are opt-in.** RESOLVED 2026-11-03: every pc/hal
  file with data in a state must be KEEP or named in `MACHINE_FILES`
  (`StateOwnershipIsListed`); a new module fails the test until someone
  decides. Still open: the decision itself is a person's, and a file
  already listed can grow a host resource -- `HostResourcesNotInStates`
  remains the check for that (extend its environment when a feature adds
  host resources). Files outside pc/hal (pc/src, src/main/pc-port) are game
  data and not in the rule.
* **R2 — C1/C4 (audio after a restore)** stay until the audio hooks
  land (section 5).
* **R3 — lost requests.** RESOLVED 2026-11-03: both are said (notes).
* **R4 — session lists across a load.** The backlog and a playing voice
  replay are the session's: after loading another save they show the other
  time line (by design; a separator line would help).
* **R15 — the interactive rewind's own event loop** does not release the
  mouse grab (A6) and leaves mouse events queued until it ends (they are
  then delivered: a click made during a rewind reaches the game). Low
  impact; the rewind holds the boundary only while its key is held.
* **R16 — a hotkey's toast during a pause** (volume, filter) is timed in
  presented frames and mostly expires under the pause label; the change
  itself is shown by the menu. Cosmetic.
* **R5 — watchdog in menu callbacks.** `qs_entries` / `qs_delete_id` join the
  save writer from inside the pause loop; a write slower than 5 s lets
  SIGALRM longjmp out of the menu (Linux).
* **R6 — `cd_audio.c` window** (C9) on a parity → real-time load.
* **R7 — quicksave / rewind while a memory-card transfer is in flight.**
  VERIFIED, RESOLVED 2026-11-03: requests wait for `memcard_busy()`, the
  rewind skips snapshots and does not start then. Left: the toolkit's own
  `save` / `load` / F5 / F9 (developer paths, bit-exact replays) do not
  wait; after `QS_CARD_LIMIT` boundary calls a request goes ahead anyway.
* **R8 — quick text vs auto-advance**: while quick text holds a press back,
  auto-advance sees no player press and may press for a player who still
  holds the button.
* **R9 — the text assist's mouse pulse shares the left button's bit** with
  the physical button.
* **R10 — `backlog_capture.c` `g_stream.open`** is not reset while the
  backlog is off; turned back on, a later sector can continue an old line.
* **R11 — `seen_text.c`**: a torn 8-byte record after a crash misaligns the
  file.
* **R12 — setup leftovers** after a kill (the lock directory, stage
  folders); the verify thread is not joined if its row is never redrawn.
* **R13 — rewind sticky statics.** RESOLVED 2026-11-03 (`RewindMachine`).
* **R14 — language across a load**: a state of the other language switches
  the session's text language (2.1). Recording the language in the slot
  index and warning on a mismatch would make it visible.

---

## 7. Tests

New: `pc/tools/test_state_machines.py`, `pc/tests/save_auto_harness.c`.

| Class | Kind | What |
|---|---|---|
| `SaveAutoMachine` | C harness (stubs, no SDL) | the save_auto machine; 4 source mutations as negative controls |
| `HostResourcesNotInStates` | port, enhanced + 2× + texture dump | the audit is clean (Linux only: on Windows the heap sits at low addresses where clock counts and data words look like pointers); another process loads the state (both platforms) |
| `EveryRestoreTellsEveryObserver` | port, `PC_RESTORE_LOG=1` | toolkit `load`, `PC_LOADSTATE`, `qload`, `rewind` |
| `QuitDuringAGame` | port, new_game.txt absolute input | close during rewind with a busy writer; no frame after the resume |
| `OneOverlayAtATime` | port, scripted keys | Ctrl+S over the backlog |
| `PacedToolkitLoad` | port, paced | toolkit `load` of a later moment keeps real time |

Second round: `pc/tools/test_ui_state_machines.py`, `pc/tests/
present_notes_harness.c`, `pc/tests/rewind_harness.c`, and new test inputs
(`TOKI_INPUT_SCRIPT` `keydown` / `keyup` / `mbutton` / `close`, `dump`'s
`mouse=`; the toolkit's `keys` and `winclose`; `TOKI_SETUP_SCRIPT` on the
ready screen; `TOKI_QS_HOTKEYS=1`).

| Class | Kind | What |
|---|---|---|
| `HoldEndsCleanly` | port, scripted keys | A2, A3, A6, A14 |
| `SettingsAndQuit` | port, toolkit keys | A9 (pinned), A11 |
| `SaveRequestsAreReported` | port, toolkit | R3 |
| `QuicksaveWaitsForTheCard` | port, new_game.txt absolute input | R7 (the state's card driver is idle) |
| `SetupReadyScreen` | port, a fresh install with the disc | A5 (and the next launch) |
| `StateOwnershipIsListed` | port inventory + mutations | R1 |
| `PresentNotes` | C harness, 2 mutations | A8 |
| `RewindMachine` | C harness, 3 mutations | R7 (rewind), R13 |

`STATE_MACHINES_BINARY=PATH` runs the port classes against another build
(the negative controls above); `STATE_MACHINES_KEEP=1` keeps their folders
(under `build/tmp/`); `PC_TEST_PLATFORM=windows` runs them on the Windows
build under Wine.

**Before / after.** The three playthroughs (`new_game`, `date_save`,
`spring_events`), each resolved to its absolute input, run under both
profiles by the build before this work (`ef5bd6be04`) and the build after it
(with origin/main of 2026-11-03 merged), with the per-VBlank trace
(`PC_TRACE`), the frame hashes (`PC_FRAME_HASH`) and the mixed-audio hashes
(`PC_AUDIO_HASH`) on, in the deterministic audio mode (`PC_PARITY_DIR`): all
18 artifacts byte-identical (5,008 / 15,903 / 30,735 parity frames, 3,603 /
10,897 / 24,603 enhanced). Without `PC_PARITY_DIR` the audio and frame hashes
differ between two runs of the same binary (the real-time mixer follows the
host clock), so that mode cannot be compared. The full battery
(`pc/tools/run_battery.sh`) passes before and after.

Second round, the same way: the hooks-only build (`e7e60f76d4`) against
the build with every fix of the second round: all 18 artifacts
byte-identical again (5,008 / 15,903 / 30,735 parity frames, 3,603 /
10,897 / 24,603 enhanced). The playthroughs never pause, open a menu or
press a quicksave key; the input path they do take (`pad.c`'s poll, with
the hold-off now judged by the keys down) gives the same pad words. The
full battery passes on the merged branch (1,226 unit tests, `make test`
60/60, scene matrix OK=63, 0 fault folds, the seven audits, playthroughs
29 / 56 / 50 milestones); `test_ui_state_machines.py` passes 16/16 on the
Windows build under Wine.
