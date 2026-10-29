# Milestones, status and what was actually run

Version 0.2.0 (preview), 28 October 2026.

## The short version

* The whole application is written: shell, shared setup profile, MouseFit, FrameFit, MotionFit, History,
  Settings, the Windows platform layer and the packaging. The interface is a plain Windows window in the style
  of Raw Accel, light or dark (or following Windows): tabs along the top (Mouse, History, Nerd stats, Settings,
  plus Frame rate and Monitor when the extra tools are switched on), ordinary text boxes, check boxes, radio
  buttons and buttons, a status bar, and no dropdown menus or pop-ups. The Mouse tab holds the whole MouseFit
  flow on one screen; Nerd stats holds every statistic and advanced control.
* Settings: theme (Same as Windows, Light, Dark), mouse model, DPI and sensitivity, quick-check reminders,
  Measure again / Quick check / Stop using my fix, Open FitSuite when Windows starts (off by default), start in
  the notification area, keep running when the window closes, the optional tools, game sensitivity compared
  with VALORANT, Check my setup, About.
* 0.2.0: the calibration is played in five short rounds like an aim trainer (flicks, micro-adjustments,
  flicks, tracking, target switching; click to start each round, the next target appears the moment the last
  one is hit or missed, a miss makes it disappear, every target has a timer), with an aim report per round. The
  blind test and the quick check use the same format. A first-run setup covers the mouse, the monitor's refresh
  rate, Raw Accel (found, or downloaded from its official release, checked by SHA-256 and installed with
  Windows' permission prompt), startup and updates. FitSuite updates itself from its public GitHub releases
  (checked against the published SHA-256), and a GitHub workflow builds, tests and publishes each tagged release.
* The quick check (about a minute, 48 targets with the fix in use) tells you whether your fix still fits or
  your aim has changed. Its decision rule was calibrated with simulated players (see the table below).
* The Windows build redraws only while something changes (otherwise about 10 times a second, and a few times
  a second while it is behind a game), caps its own interface at about 60 frames per second on high-refresh
  monitors, saves the setup at most once a second while you edit, and, hidden in the notification area, holds
  no Direct3D device, runs in Windows efficiency mode at below-normal priority with its memory trimmed, and
  wakes only for program switches (while the per-game fix is on), Raw Accel's writer, and a once-a-minute
  reminder check. These are Windows-only and have not been measured on Windows yet.
* The math core is covered by 85 unit tests (2,113,101 assertions), all passing.
* A refresh-rate check warns when a monitor runs below the fastest rate Windows offers, with a one-click
  switch that uses the existing 15-second keep-or-revert.
* MouseFit's fix can be switched on only while chosen games are in front (per-game switching through Raw
  Accel, with FitSuite waiting in the notification area). The switching rules are unit-tested and the whole
  flow ran in the development harness with a stand-in for Raw Accel's `writer.exe`; it has not run on Windows.
* The real UI code was exercised end to end in a Linux development harness with a **simulated**
  player, simulated PresentMon files and a simulated observer.
* `FitSuite.exe` was cross-compiled for 64-bit Windows and inspected, but **it has not been run on
  Windows**. No Windows machine was reachable from the build environment. Nothing that touches real
  hardware (raw mouse input, Direct3D rendering, refresh-rate switching, DDC/CI, PresentMon, Raw Accel,
  VALORANT) is verified yet.

## What was actually run

| What | Where | Result |
|---|---|---|
| Unit tests (`fitsuite_tests`) | Linux x86-64, clang 18, Release | 85 test cases, 2,113,101 assertions, all pass (~13 s) |
| Round-based tests (0.2.0) | unit tests with the simulated player playing rounds (one shot per target, leftover hand movement when a target appears, tracking with a reaction lag) | round calibration finds a 10% vertical undershoot 12/12 and proposes nothing for an unbiased player 57/60; settling movement shifts the measured distance error by 0.2 points; misses count as a full timer; blind test in rounds: identical candidate "improved" 1/200, real correction 24/30, harmful candidate 0/20; quick check in rounds: unchanged 6/150, capped fix 4/120, new downward undershoot noticed 85/120; tracking names the weaker direction (100 vs 40 ms behind) and none for an unbiased player; sessions with rounds and tracking round-trip through their file format |
| Round-based tests in the harness | same `app/` code, simulated player | full calibration (5 rounds: intro cards, flicks with timer rings, micro, tracking with on-target colouring, switching with 3 targets), aim report on the Mouse tab, an 8-block blind test and a quick check, all completing; screenshots reviewed |
| First-run setup in the harness | same harness, the real Raw Accel 1.7.1 zip as the "download", stand-ins for the installer and the restart | every step rendered in light and dark; the zip passed the SHA-256 check, unpacked, the installer ran, "restart" shown; after the simulated restart "Open Raw Accel" until a settings.json existed, then OK; the Visual C++ warning shown when the runtime is missing |
| Updater in the harness | same harness, a fake release JSON and files | "v0.3.0 available" shown (status bar and Settings); a download with a wrong checksum refused; the right one "downloaded and checked"; "Update now" handed the file to the platform and asked FitSuite to quit |
| Release build with the CI toolchain | Linux, official llvm-mingw 20240619 (the version the GitHub workflow uses), `tools/package.sh` | builds with zero warnings and packages `FitSuite-0.2.0-preview-win64.zip`; same system-DLL imports |
| Refresh-rate check in the harness | same harness, simulated display at 60 Hz offering 60/120/144/240 Hz | warning shown on the Mouse tab and "Check" in Check my setup; **Switch to 240 Hz** changed the rate and opened the keep-or-revert box; left unanswered, it went back to 60 Hz after 15 s and History showed the change as REVERTED; **Keep 60 Hz** hid the warning |
| Per-game switching in the development harness | Linux, Xvfb, same `app/` code as Windows, simulated foreground program and a stand-in `writer.exe` that only records the file it is given | turning it on with FitSuite in front applies your own settings at once; VALORANT in front: nothing for 1 s, then the corrected file; a 0.4 s Alt-Tab to a browser and back: no switch; a story game in front for over 1 s: back to your own settings; Counter-Strike 2 added to the list: on; FitSuite brought to the front while the fix was on: off at the next check, and a calibration started in that moment was refused until Raw Accel confirmed; a writer failure: switching paused with an explanation; quitting: your own settings applied. Re-run after the Settings changes: VALORANT → corrected file, browser → your own file, turning it off and on with a browser in front → your own file at once, the startup setting untouched by either |
| Quick check decision rule | inside the unit tests, simulated players (`tests/test_check.cpp`) | unchanged player: "changed" 7/150; fix capped at its bounds: 2/120 (106/120 without the expected-leftover correction); unbiased player without a fix: 3/150; new 12% downward undershoot: noticed 83/120; doubled vertical undershoot: noticed 53/60 |
| Quick check, reminders, Settings in the harness | same harness | quick checks with the simulated "strong" player (fix capped) said "still fits" three times; a player whose downward flicks changed got "changed" with the direction named; a fix last checked 10 days ago gave one reminder and then none the same day; Check my setup listed every item, flagged "Open when Windows starts" when it was off while the fix switches per game and when the entry was switched off as if in Task Manager, and cleared both after the box was ticked again; ticking the startup setting added an entry under History (APPLIED, with a **remove** link) and unticking it marked the entry REVERTED |
| Light, dark and "Same as Windows" themes | same harness, 1440 × 920 window | screenshots of Mouse, History, Nerd stats and Settings in light and dark, and Settings following a simulated Windows dark mode, were reviewed; a truncated column in Settings → Game sensitivity was found and fixed |
| Statistical calibration of the decision rules | inside the unit tests, simulated players | no-bias player: correction proposed in 4/120 calibrations; identical candidate: "improved" 3/200, "worse" 4/200; noisy player with drift: "improved" 5/120; real 10% vertical undershoot with exact correction: "improved" 20/30 at the standard length; downward-only 12% undershoot: Raw Accel candidate boosts downward movement only (down +6.3%, up −1.6%, capped by the bound) |
| UI flows in the development harness | Linux, Xvfb, Mesa software OpenGL, same `app/` code as Windows | calibration (4–6 blocks), candidate review, blinded comparison (8, 12, 16 blocks; both "keep your baseline" and "candidate recommended" outcomes), apply/export screen (including writing a corrected Raw Accel settings file from a sample settings.json), FrameFit with synthetic PresentMon 2.x CSVs (3 configurations × 3 runs, comparison and recommendation), MotionFit blinded refresh-rate identification test (60/144/240 Hz) and trail ratings with a simulated observer, History. |
| The light Raw Accel-style layout | same harness, 1440 × 920 window | screenshots of every tab and state were reviewed: Mouse before measuring, after measuring, after a blind test, with the fix chosen, with the fix on in VALORANT, and with the "Use the fix everywhere?" confirmation; Frame rate with runs and a recommendation; Monitor after a blinded refresh-rate test; History; Nerd stats → Mouse (Validate, Apply), Frame rate, Monitor and System; the dark full-screen test scenes. No dropdowns, pop-ups or collapsible sections remain in the interface code. |
| Windows build | Linux → Windows cross-compile with llvm-mingw (clang 18, UCRT, static C++ runtime) | `FitSuite.exe` builds with zero warnings |
| Executable inspection | `llvm-readobj`, `llvm-objdump -p`, `strings`, on the stripped exe in the zip | x64 GUI subsystem; ASLR, DEP and high-entropy VA enabled; embedded manifest (per-monitor DPI, runs as invoker, Windows 10/11); icon and version resources (0.1.0); imports only Windows system DLLs (kernel32, user32, gdi32, d3d11, dxgi, d3dcompiler_47, dxva2, hid, dwmapi, shell32, comdlg32, ole32, advapi32, winmm, winhttp and the Universal CRT), including the background functions `SetWinEventHook`, `SetProcessInformation`, `SetPriorityClass`, `SetProcessWorkingSetSize` and `MsgWaitForMultipleObjectsEx` |

**Not run:** the Windows executable itself; a Visual Studio (MSVC) build; Raw Accel's `writer.exe` applying a
FitSuite settings file ("Apply now", "Make it my default", "Restore", per-game switching); the Windows parts of
per-game switching (the program-switch event hook, reading the program in front, the notification-area icon
and reminders, closing to it, the startup entry, Task Manager's switch for it, and the hidden `--autostart`
start); restoring your Raw Accel settings when Windows shuts down; the round-based tests with a real mouse;
downloads over WinHTTP, unpacking with Windows' tar, Raw Accel's installer behind the permission prompt, and
replacing and restarting FitSuite.exe during an update; the GitHub release workflow itself; the refresh-rate list and switch on real monitors; efficiency mode, priority and memory
trimming while hidden; the idle redraw, the 60 fps interface cap and releasing Direct3D while hidden; the dark
title bar and following Windows' app mode; PresentMon launched by FitSuite; anything in VALORANT.

## Build order

| # | Step | Status |
|---|---|---|
| 1 | Plan and feasibility | Done: `PLAN.md` (capability table, output-path investigation). |
| 2 | Stack | Done: C++20, Win32, Direct3D 11, Dear ImGui + ImPlot, CMake. |
| 3 | Desktop shell + shared profile model | Done. Versioned JSON setup profile, validation with path-tagged errors, atomic saves, 10 rotating backups, automatic recovery from a damaged file, staleness detection when hardware or settings change. |
| 4 | Raw input + target test | Written; **not verified on Windows**. Per-device raw input on its own thread with QPC timestamps, injected/absolute input rejected, capture only while focused, immediate release on focus loss, Esc stops. |
| 5 | Directional measurement, fitting, blinded comparison | Done and unit-tested; UI flow exercised with simulated input. |
| 6 | FrameFit | Done: PresentMon launcher (Windows, unverified), CSV import for three PresentMon formats (tested), metrics, run-to-run noise flags, Welch comparisons, goal-based recommendation. |
| 7 | MotionFit | Done: blinded refresh-rate identification test, manual overdrive comparison, trail ratings, psychometric analysis (tested). Refresh switching on Windows unverified. |
| 8 | Shared profiles | Done: one setup profile feeds all three tools (DPI/sensitivity, monitor and refresh, FrameFit's measured frame rate in MotionFit), baseline snapshot, change log. |
| 9 | Settings application and restoration | Done within what is possible: Raw Accel settings export (composed with your existing Raw Accel values), applying it with Raw Accel's own `writer.exe`, making it the default after backing up `settings.json`, switching it per game by the program in front (your `settings.json` untouched), and restoring (Windows parts unverified); refresh-rate change with a 15-second keep-or-revert, temporary test changes that Windows itself reverts, restore-to-baseline for everything FitSuite controls. Game and monitor-menu settings are guided, never claimed as applied. |
| 10 | Windows package | Done: `FitSuite-0.1.0-preview-win64.zip` (single exe, no installer, no runtime downloads). |

## What works (as far as it could be checked here)

* Every page and flow renders and behaves as designed with simulated data, including the "default to
  baseline" outcomes, the out-of-range and noisy-data warnings, and labelling of every value
  (MEASURED / ESTIMATE / PREFERENCE / ENTERED / DETECTED / SIMULATED DATA).
* Profile storage, validation, recovery and import/export.
* All analysis: segmentation, fitting, comparison statistics, frame metrics, psychometric fits.

## Blocked or not possible yet

| Item | Why |
|---|---|
| Applying the smooth directional profile in games | Needs a custom, attestation-signed kernel filter driver. Not built. Raw Accel can only apply the rotation + axis-ratio family, which FitSuite fits and validates separately. |
| Confirming Raw Accel works with VALORANT | Raw Accel's FAQ says so; not verified by us. |
| Recognising "a shooter" automatically | Windows has no genre information for a program. FitSuite recognises 14 popular shooters by executable name and lets you add any game; it does not guess. |
| Writing profiles to mouse firmware | Vendor-specific, no public APIs; deliberately not attempted. |
| End-to-end mouse-to-photon latency | Needs an external light sensor. FitSuite reports PresentMon's latency columns with their exact definitions only. |
| Changing VALORANT settings | No supported API; FitSuite guides you and records your confirmation. |
| Overdrive control | No standard DDC/CI code; you switch it in the monitor menu. |
| Measuring pixel response / ghosting | Not observable by software from rendered frames; would need a camera or photodiode method. |
| Code signing | The executable is unsigned, so Windows SmartScreen will warn on first launch. |

## How to run

* Players: see "Quick start" in `README.md` (unzip, run `FitSuite.exe`).
* Developers: see "Building from source" in `README.md`. Tests: `cmake -S . -B build && cmake --build build && build/fitsuite_tests`.

## First things to check on a real Windows PC

1. On first start the setup opens, in the Windows font and sharp at your display scaling. The monitor step shows
   your refresh rate (or offers the faster one). Settings → Theme: Light, Dark and Same as Windows switch the
   window and its title bar.
2. Setup → Raw Accel → "Download and install Raw Accel": the download finishes, Windows' permission prompt
   appears, Raw Accel's installer runs, and after a restart the setup continues and "Open Raw Accel" opens it;
   after you click Apply there, the step shows OK.
3. Measure my flicks: each round starts on a click, targets appear immediately after each shot, misses vanish,
   timer rings empty, tracking and switching rounds play smoothly; the pointer is hidden and released when you
   Alt-Tab or press Esc; afterwards box 1 shows the aim report. The test-scene line reports a frame rate near
   twice your refresh rate.
4. Per-game switching: Mouse tab → 2 Use it in your games → tick VALORANT → "Turn on". Start VALORANT: within
   about two seconds Raw Accel shows the corrected values (open its window afterwards to check); Alt-Tab to the
   desktop for a few seconds: your own values. Close the FitSuite window: its icon stays in the notification
   area, and right-click → Quit puts your own values back.
5. Startup: tick Settings → "Open FitSuite when Windows starts"; Task Manager → Startup apps lists FitSuite.
   Restart: FitSuite starts hidden in the notification area and Raw Accel shows your own values until VALORANT
   is in front. Disable FitSuite in Task Manager → Startup apps: Settings shows the warning and Check my setup
   says "Check"; untick and tick the box: enabled again. Untick it: the entry is gone.
6. Task Manager: with FitSuite open and untouched, CPU and GPU use stay near zero; closed to the notification
   area, FitSuite shows the efficiency-mode leaf, no GPU memory and a small working set. Reopening it from the
   icon draws the window normally.
7. Settings → Your mouse → Quick check runs 48 targets with the fix in use and gives a verdict; Settings →
   Check my setup shows OK for everything you have set up.
8. With Settings → Extra tools ticked, the Frame rate tab finds PresentMon (put it in a `tools` folder next to
   `FitSuite.exe`) and records a run in the Range.
9. Nerd stats → Monitor → "check" DDC/CI, and a blinded refresh-rate test on the Monitor tab switches rates and
   restores yours at the end.
10. History → "Changes made by FitSuite" lists every change, and the restore actions work.
11. Updates: once a newer release exists, Settings → Updates shows it; "Download the update" then "Update now"
    restarts into the new version, and FitSuite.exe.old disappears from the folder.
12. Nerd stats → Mouse → 5 Apply → "Create a Raw Accel settings file", "Apply now" and "Restore" work, and Raw
    Accel shows the new values.
