# FitSuite

FitSuite is a Windows desktop app that tunes your setup for a competitive shooter from measurements on your
own PC, starting with VALORANT. Its main tool is the mouse fix; two optional tools share the same setup profile:

* **MouseFit** measures how your first flick lands in each direction, fits a small, bounded per-direction
  correction for any mouse, and only recommends it if a blinded comparison shows it makes you faster
  without costing accuracy. The measurement plays like an aim trainer: short rounds of flicks,
  micro-adjustments, tracking and target switching. Through Raw Accel it can switch that fix on only while your
  games are in front, from the background, and a one-minute quick check tells you when your aim has changed
  enough to measure again.
* **FrameFit** (optional) records how evenly your PC delivers frames in VALORANT (with Intel PresentMon) and
  compares settings by repeated runs, not single numbers.
* **MotionFit** (optional) compares refresh rates (blinded) and overdrive settings by how well you can read
  moving details on your own monitor.

The optional tools are hidden until you tick **Settings → Extra tools**.

Everything stays on your computer. There is no account, subscription, cloud service or AI service. FitSuite
goes online only to check for its own updates (once a day, can be switched off) and, when you click it, to
download Raw Accel during the setup.

> **Preview status.** Version 0.2.0 has been built and tested with simulated data, but it has **not yet been
> run on a Windows PC**. Treat it as a preview and see [docs/MILESTONES.md](docs/MILESTONES.md) for exactly
> what was checked.

---

## Quick start (Windows 10 or 11, 64-bit)

There is nothing to install and no local server to set up: FitSuite is a single program.

1. Download `FitSuite-0.2.0-preview-win64.zip` (from the
   [releases page](https://github.com/tsbforlife123/FitSuite/releases) once it is published).
2. Right-click it → **Extract All…** and pick a folder, for example `Documents\FitSuite`.
3. Double-click **FitSuite.exe**.
   The preview is not code-signed, so Windows may show "Windows protected your PC". Click **More info → Run
   anyway**. FitSuite does not need administrator rights.
4. The first time, a short **setup** opens: your mouse (DPI, VALORANT sensitivity, model), your monitor's
   refresh rate, Raw Accel (FitSuite can download and install it for you), startup and updates. Then click
   **Measure my flicks**. Everything the setup asks is also on the **Settings** tab later.
5. Later versions install themselves: **Settings → Updates → Download the update → Update now**.

### The window

One plain window, like Raw Accel, in light or dark: tabs along the top (**Mouse**, **History**, **Nerd stats**,
**Settings**, plus **Frame rate** and **Monitor** when the extra tools are on), ordinary Windows text boxes,
check boxes and buttons, and a status bar at the bottom. There are no dropdown menus or pop-ups; a choice is a
check box or a radio button, and the few actions that need a second thought (making the fix your Raw Accel
default, stopping your fix, deleting a run) ask once, in place.

* **Mouse** holds the whole MouseFit flow on one screen: *Your mouse*, *1 Measure your flicks* (with your aim
  report), *2 Your fix*, *3 Blind test*, *4 Use it in your games* and *5 Check your fix*, with the direction
  chart on the right.
* **History** lists your saved profiles and every change FitSuite made, each with a way back.
* **Nerd stats** holds everything else: every measurement, chart, confidence interval and model detail, plus all
  the advanced controls.
* **Settings** is a short list of options (below).
* **Frame rate** (FrameFit) and **Monitor** (MotionFit) are the optional tools.
* The status bar shows whether the mouse fix is on (and in which game), whether Raw Accel is installed, and the
  latest message.

### Settings

| Section | What is there |
|---|---|
| Appearance | Theme: **Same as Windows** (follows Windows' light or dark app mode, also while FitSuite is open), **Light** or **Dark**. The title bar follows. |
| Your mouse | Mouse model (typed by you; the detected device name is shown below it), DPI, VALORANT sensitivity and the distance of a full turn; how often to remind you of the quick check (every week, every month, never); **Measure again**, **Quick check** and **Stop using my fix**. A different mouse model, DPI or sensitivity marks your fix as measured under other conditions. |
| Startup and background | **Open FitSuite when Windows starts** (off until you tick it), **Start in the notification area, without opening the window** (on), **Keep running in the notification area when I close the window** (on; always on while your fix switches per game). |
| Extra tools | **Show the Frame rate and Monitor tabs** (off). |
| Game sensitivity | Your sensitivity in the other games you ticked, converted to the mouse distance of a full turn and compared with your VALORANT setting (see below). |
| Check my setup | One button that checks each monitor's refresh rate, Raw Accel's driver, `settings.json` and `writer.exe`, your mouse, the startup entry (including whether Task Manager has switched it off), the per-game switching and the data folder, and says what is missing. |
| About | Version, where your data is, **Open data folder**. |

### Is your monitor running at full speed?

A 144 or 240 Hz monitor left at 60 Hz in Windows is a common setup mistake. When a monitor runs at least 5 Hz
below the fastest rate Windows offers at its current resolution, the Mouse tab (and the Monitor tab) shows a
warning with **Switch to … Hz** (15 seconds to keep it or go back, listed under History) and **Keep … Hz** (no
more warnings for that monitor until a faster rate appears). Windows only offers rates that the monitor, cable
and graphics card all accept, so if your monitor is sold as faster than anything listed, check the cable and port.

### Measuring your aim: rounds like an aim trainer

**Measure my flicks** plays five short rounds (about 4 minutes): flicks, micro-adjustments (small, close
targets), flicks again, tracking (keep the crosshair on a moving target for 40 seconds) and target switching
(three targets at a time). Click to start each round. From then on a new target appears the moment you hit or
miss the last one, a miss makes the target disappear, and every target has a timer ring. Afterwards box 1 shows
an aim report per round (hits, time per target, tracking time on target and the direction you fall behind in
most). The fix itself is fitted from where your first flick toward each target ends; how is described in
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md). The blind test (8 short blocks, about 2 minutes) and the quick
check (about a minute) use the same format.

### First-run setup

Shown the first time FitSuite opens (and from **Settings → About → Run the setup again**): your mouse, your
monitor (it offers the faster refresh rate if Windows is set lower), Raw Accel, startup and updates. For Raw
Accel it finds an existing install, or downloads the official Raw Accel 1.7.1 release from GitHub, checks it
against that release's SHA-256, unpacks it to `%LOCALAPPDATA%\Programs\RawAccel` and starts its installer;
Windows asks for permission, and a restart finishes it. After the restart the setup continues and helps you
open Raw Accel once so it saves its settings. Every step can be skipped.

### Updates

With **Check for updates once a day** on (the default; Settings → Updates), FitSuite asks GitHub for the
latest release of [tsbforlife123/FitSuite](https://github.com/tsbforlife123/FitSuite/releases). When there is a
newer version the status bar says so. **Download the update** fetches it and checks it against its published
SHA-256; **Update now** swaps in the new `FitSuite.exe` and restarts. Your settings and measurements stay as
they are. If FitSuite's folder is not writable (e.g. inside Program Files), it says so and you download the new
zip yourself. Releases are built from the tagged source by the repository's GitHub workflow.

### Running in the background

Closing the window keeps FitSuite in the notification area (unless you untick that in Settings and the per-game
fix is off); right-click its icon to open or quit it. While it waits there it draws nothing and holds no
graphics device, asks Windows for efficiency mode and below-normal priority, and hands unused memory back. It
wakes up only when there is something to do: when a different program comes to the front while your fix
switches per game (Windows tells it; it also checks every 2 seconds in case a notification is missed), every
0.1 seconds while Raw Accel's writer is applying settings (to see when it is done), and once a minute to see
whether a quick-check reminder is due. With the window open and untouched it redraws about 10 times a second,
and a few times a second while it is behind a game. These behaviours are Windows-only and have not been
measured on a Windows PC yet.

### Quick check (Mouse tab → 5 Check your fix)

About a minute of flicks (three short rounds, 48 targets in 8 directions) with your fix in use. FitSuite looks for any directional
bias that is left and either says the fix still fits or tells you which direction has changed and that it is
time to measure again. It never changes your fix itself; a new fix always comes from a full measurement and a
blind test. You get a reminder every week (or month, or never: Settings → Your mouse): in the app, and as a
Windows notification at most once a day while FitSuite waits in the notification area, never while your fix is
on in a game. How the check decides is described in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

### Optional: PresentMon (for FrameFit)

FrameFit uses Intel's free PresentMon console application to read frame timing.

1. Download `PresentMon-2.x.x-x64.exe` from the PresentMon releases page on GitHub
   (github.com/GameTechDev/PresentMon/releases). FitSuite has a link to it on the Frame rate tab.
2. Tick **Settings → Extra tools → Show the Frame rate and Monitor tabs**.
3. Make a folder called `tools` next to `FitSuite.exe` and put the PresentMon file in it, or point FitSuite
   to it with **Find it…** on the Frame rate tab.
4. PresentMon needs administrator rights to read graphics events. Either tick **Run PresentMon as
   administrator** when you record (Windows will ask), or add your account to the "Performance Log Users"
   group once and sign out and back in.

### Optional: Raw Accel (to use a MouseFit correction in games)

A correction validated in MouseFit can be used outside FitSuite only through Raw Accel, a free signed mouse
driver, and only in its "Raw Accel-compatible" form: a small rotation plus separate sensitivity for up, down,
left and right. If your downward flicks land short, for example, Raw Accel then adds a few percent of
sensitivity whenever the mouse moves down. It reacts only to the direction you move, never to the screen or
the game.

**Only in your games (Mouse tab → 4 Use it in your games).** Tick the games where the fix should work (VALORANT
is ticked; 13 other shooters are listed, and you can add any game while it is running), then **Turn on**, or
click **Use this fix in my games** right after a blind test it passed. FitSuite switches Raw
Accel to your settings plus the fix while one of those games is in front, and back to your own settings for
everything else: desktop, browser, story games. It knows which game is in front only from the program's name;
it never looks into a game. FitSuite has to keep running for this, so closing the window keeps it in the
notification area. **Turn off** puts your own settings back.

Raw Accel's driver does not keep settings over a restart (its FAQ: "The driver itself does not store your
settings"). Tick **Open FitSuite when Windows starts** (in the same box, or in Settings) and FitSuite starts
hidden at sign-in, puts your own Raw Accel settings on and switches the fix per game again. Without it, the fix
stays off in your games after a restart until you open FitSuite; **Settings → Check my setup** points this out.

The fix rides on top of each game's own sensitivity: at most 8% more or less in a direction, unchanged on
average, so nothing gets faster overall. It was measured with your VALORANT sensitivity, so it carries over best
to games where you use about the same distance for a full turn. **Settings → Game sensitivity** shows that
distance for each game you ticked: type your in-game sensitivity (Counter-Strike 2, Apex Legends, Overwatch 2 and
Call of Duty are converted with their published turn rates) or, for other games, the centimetres of mouse
movement for one full turn. Games more than 15% faster or slower than your VALORANT setting are named on
the Mouse tab.

**Everywhere instead.** The same box can also make the fix your Raw Accel default (**Use it everywhere
instead…**, after saving a copy of your `settings.json`); then it is always on and FitSuite does not need to run.
Nerd stats → Mouse → 5 Apply has the manual options ("Apply now" until restart, a settings file to load
yourself, restore).

Raw Accel's FAQ says it works with VALORANT; FitSuite has not verified that. Installing Raw Accel needs
administrator rights and a restart.

### Where your data lives

* `%LOCALAPPDATA%\FitSuite` (paste that into the Explorer address bar). **Settings → About → Open data folder**
  (or the same button on History) opens it.
* For portable use (e.g. from a USB stick), create a folder named `FitSuiteData` next to `FitSuite.exe`;
  FitSuite then keeps everything there.
* Files are versioned JSON, saved atomically with 10 backups. A damaged file is set aside and the newest good
  backup is used.

### Removing FitSuite

If the per-game mouse fix is on, turn it off first (Mouse tab → 4 Use it in your games → **Turn off**): that
puts your own Raw Accel settings back. Untick **Settings → Open FitSuite when Windows starts** to remove its
startup entry. Then quit FitSuite (right-click its notification-area icon → Quit) and delete the folder you
extracted and `%LOCALAPPDATA%\FitSuite`. If you applied a refresh rate with FitSuite, change it back in Windows
display settings (the History tab lists every change FitSuite made). Raw Accel is a separate program: remove it
with `uninstaller.exe` in its folder (`%LOCALAPPDATA%\Programs\RawAccel` if the setup installed it).

---

## What FitSuite does and does not do

* It reads raw mouse input only while one of its own tests is running and its window is in front. Leaving
  the window or pressing Esc releases the mouse immediately.
* During your games it does at most one thing, and only if you turned on MouseFit's per-game fix: when Windows
  reports that a different program came to the front (an out-of-context event hook, so nothing is loaded into
  other programs), it reads that program's name and switches Raw Accel between your own settings and your
  settings with the fix. It never moves the mouse or presses keys, never opens the game's process, never reads
  game memory or the screen, never reacts to anything in a match, and never touches anti-cheat. Without the
  per-game fix it does nothing while you play; at most it waits in the notification area and wakes once a
  minute to see whether a quick-check reminder is due.
* It never edits the registry for "tweaks", disables services or changes game files. The only things it can
  change, and only when you click, are your monitor's refresh rate (with a 15-second keep-or-revert), your Raw
  Accel settings (through Raw Accel's own tool, after saving a copy) and, if you tick "Open FitSuite when
  Windows starts", one per-user startup entry (and, if Task Manager had switched that entry off, its switch).
  Each change goes into a change log with a way back. Refresh-rate switches during a test are temporary and
  undone automatically. When Windows shuts down or signs you out, FitSuite puts your own Raw Accel settings back.
* It changes no VALORANT settings. It guides you and records that *you* changed them.
* Every number is labelled MEASURED, ESTIMATE, PREFERENCE, ENTERED, DETECTED or SIMULATED DATA.
  Frame time is never presented as mouse-to-screen latency, and monitor results are shown as what you saw, not
  as a measurement of the panel.

The exact definitions are in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md); the design and the feasibility
research (including why a full smooth mouse correction cannot be applied in games yet) are in
[docs/PLAN.md](docs/PLAN.md).

---

## Building from source

Requirements: CMake 3.20 or newer and Ninja, plus one of these compilers:

| Toolchain | Status |
|---|---|
| llvm-mingw (clang, UCRT) | Used for the release build (cross-compiled from Linux). |
| Visual Studio 2022, "Desktop development with C++" | Expected to work; not tested yet. |

All third-party code (Dear ImGui, ImPlot, nlohmann/json, doctest) and the fonts are included in the
repository; the build downloads nothing.

**On Windows** (PowerShell, from the project folder):

```powershell
# with llvm-mingw: download a "...-ucrt-x86_64.zip" release from github.com/mstorsjo/llvm-mingw and
# extract it, for example to C:\tools\llvm-mingw
powershell -ExecutionPolicy Bypass -File .\tools\build-windows.ps1 -LlvmMingw C:\tools\llvm-mingw

# or with Visual Studio: open "x64 Native Tools Command Prompt for VS 2022" in the project folder, then
powershell -ExecutionPolicy Bypass -File .\tools\build-windows.ps1
```

The program is written to `build\FitSuite.exe`. Add `-Tests` to also build and run the unit tests.

**Cross-compiling on Linux** (as used for the release):

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DFITSUITE_BUILD_APP=ON -DFITSUITE_BUILD_TESTS=OFF
cmake --build build-win
```

**Unit tests** (any platform, no Windows APIs needed):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/fitsuite_tests
```

**Development harness** (Linux, X11): renders the real UI with a simulated player so the interface can be
developed and screenshotted without Windows. Everything it shows is labelled simulated.

```sh
cmake -S . -B build-harness -DFITSUITE_BUILD_HARNESS=ON -DFITSUITE_BUILD_TESTS=OFF && cmake --build build-harness
xvfb-run -a -s "-screen 0 1600x1000x24" ./build-harness/fitsuite_harness --data /tmp/fitsuite-dev \
    dpi=800 sens=0.4 user=vertical calibrate=4 page=mouse frames=5 shot=mouse.png theme=dark page=settings \
    frames=5 shot=settings-dark.png
```

## Project layout

```
core/            math, analysis and storage; no UI and almost no OS code; fully unit-tested
app/             pages, test scenes and session flows (Dear ImGui), talking to the OS through app/platform.hpp
platform/win32/  Windows: raw input, displays, system info, PresentMon, downloads and updates, window + Direct3D 11
harness/linux/   development harness (not a product build)
tests/           unit tests and the simulated player
docs/            PLAN.md, MEASUREMENTS.md, MILESTONES.md, releases/ (release notes)
tools/           build-windows.ps1, package.sh
.github/         the release workflow: every push to main is built and tested; a new version number is published as a release
```

## Third-party software

| Component | Licence | Use |
|---|---|---|
| Dear ImGui 1.93.0 | MIT | user interface |
| ImPlot 1.1 | MIT | charts |
| nlohmann/json 3.12 | MIT | profile files |
| doctest 2.5 | MIT | unit tests (not in the executable) |
| Inter, IBM Plex Mono | SIL Open Font License 1.1 | fonts embedded in the executable |
| LLVM libc++ / mingw-w64 runtime | Apache 2.0 with LLVM exception / permissive | statically linked C++ and C runtime |
| Raw Accel | MIT | not included; FitSuite reproduces its order of operations to validate compatible corrections and writes its settings format; the setup can download the official release (checked by SHA-256) and start its installer |
| Intel PresentMon | MIT | not included; downloaded by you and launched by FitSuite |

Full licence texts are in `THIRD_PARTY_NOTICES.txt` (in the release zip, generated by `tools/package.sh`).
VALORANT is a trademark of Riot Games; FitSuite is not affiliated with Riot Games, Intel, Corsair or the
Raw Accel project.
