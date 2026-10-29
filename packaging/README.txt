FitSuite - mouse, frame and motion calibration
==============================================

This build has been tested with simulated data but has not yet been run on a Windows PC.
Please report anything that does not work as described.

START
  The easiest way is FitSuite-Setup.exe from github.com/tsbforlife123/FitSuite/releases:
  it installs FitSuite for your account (no administrator rights), with shortcuts, and
  adds Uninstall to Windows Settings > Apps. This zip is the portable version:
  1. Keep this folder anywhere you like (for example Documents\FitSuite).
  2. Double-click FitSuite.exe. No administrator rights are needed.
  3. The build is not code-signed, so Windows may say "Windows protected your PC".
     Click "More info", then "Run anyway".
  4. The first time, a short setup asks for your mouse, checks your monitor, installs Raw
     Accel if you want (download from its official release, checked, Windows asks for
     permission, then a restart), and sets startup and updates.
  Requirements: Windows 10 or 11, 64-bit, a graphics card with Direct3D 11.

UPDATES
  FitSuite checks github.com/tsbforlife123/FitSuite for a new version each time it opens
  (on by default; switch it off in Settings > Updates). When there is one, a box asks
  "Update" or "Not now". The update is checked against its published SHA-256 before it is
  used, and your settings and measurements stay as they are. What's new opens each time
  FitSuite opens until you click "Don't show again".
HOW IT WORKS
  One plain window with tabs, like Raw Accel, in light or dark. The Mouse tab has everything
  for the mouse fix on one screen: enter your DPI and VALORANT sensitivity, then
  1 Measure and fix (five short rounds, about 4 minutes: flicks, micro-adjustments,
  tracking, target switching; click to start each round, targets keep coming, one shot
  each, every target has a timer), then your fix and its blind test (about 2 minutes),
  and 2 Use it in your games. The one-minute quick check is in Settings.
  History lists every change with a way back. Nerd stats has every number, chart and
  advanced control. Settings has the options below. The status bar at the bottom shows
  whether the fix is on.

SETTINGS
  Appearance        Same as Windows, Light or Dark.
  Your mouse        Mouse model, DPI, VALORANT sensitivity, quick-check reminder (every
                    week, every month or never), Measure again, Quick check, Stop using
                    my fix.
  Startup and       Open FitSuite when Windows starts (off until you tick it), Start in the
  background        notification area (on), Keep running in the notification area when I
                    close the window (on; always on while your fix switches per game).
  Extra tools       Show the Frame rate and Monitor tabs (off).
  Game sensitivity  Your sensitivity in your other games, compared with VALORANT by the
                    mouse distance of a full turn.
  Check my setup    Checks monitor speed, Raw Accel, your mouse, the startup entry and the
                    data folder, and says what is missing.

MONITOR SPEED
  If a monitor runs slower than the fastest refresh rate Windows offers (a 144 Hz monitor
  left at 60 Hz, say), the Mouse tab shows a warning with "Switch to ... Hz" (15 seconds
  to keep it or go back) and "Keep ... Hz". If your monitor is sold as faster than
  anything Windows offers, check the cable and port.

IN THE BACKGROUND
  Closed to the notification area, FitSuite draws nothing, holds no graphics memory and
  runs in Windows' efficiency mode at low priority. It wakes only when there is work: a
  program switch while your fix switches per game, Raw Accel's writer, or the reminder
  check once a minute. Right-click its icon to open or quit it.

FRAMEFIT NEEDS PRESENTMON (free, from Intel)
  1. Tick Settings > Extra tools > "Show the Frame rate and Monitor tabs".
  2. Download PresentMon-2.x.x-x64.exe from github.com/GameTechDev/PresentMon/releases
  3. Create a folder named "tools" next to FitSuite.exe and put PresentMon in it
     (or choose it with "Find it..." on the Frame rate tab).
  4. PresentMon needs administrator rights: tick "Run PresentMon as administrator" when
     recording, or add your account to the "Performance Log Users" group and sign in again.

USING A MOUSEFIT CORRECTION IN GAMES
  Only "Raw Accel-compatible" corrections (rotation plus separate up/down/left/right
  sensitivity) can be used outside FitSuite, through the free Raw Accel driver.
  Mouse tab, "2 Use it in your games": tick your games (VALORANT is ticked; any running
  game can be added) and click "Turn on". FitSuite then switches Raw Accel to your
  settings plus the fix while one of those games is in front, and back to your own
  settings for everything else. It only reads the name of the program in front; it
  never looks into a game. FitSuite has to keep running for this, so closing the window
  keeps it in the notification area. "Turn off" puts your own settings back.
  Raw Accel's driver keeps no settings over a restart, so tick "Open FitSuite when
  Windows starts" to have the fix switching again after a restart.
  The fix works on top of each game's own sensitivity: at most 8% more or less in a
  direction, the same on average. The same box can also make the fix your Raw Accel
  default ("Use it everywhere instead...": always on, FitSuite not needed).
  FitSuite has not verified Raw Accel in VALORANT.

YOUR DATA
  Stored in %LOCALAPPDATA%\FitSuite (Settings > About > "Open data folder").
  For portable use, create a folder named FitSuiteData next to FitSuite.exe.
  Nothing is uploaded anywhere. FitSuite goes online only for the daily update check (if
  on) and, when you click it, to download Raw Accel in the setup.

WHAT FITSUITE NEVER DOES
  It never moves the mouse or presses keys, never opens a game's process, never reads the
  screen or game memory, never reacts to anything in a match, never touches anti-cheat,
  never changes services and never changes VALORANT settings. While you play, it does
  something only if you turned on MouseFit's per-game fix, and then only reads which
  program is in front. The only things it can change, when you click, are your monitor
  refresh rate (15-second keep-or-revert), your Raw Accel settings (after saving a copy)
  and, if you tick "Open FitSuite when Windows starts", one startup entry for your user.
  Every change is logged with a way back.

REMOVING
  If the per-game fix is on, turn it off first (Mouse tab > "Turn off"). Untick
  Settings > "Open FitSuite when Windows starts". Quit FitSuite (right-click its icon >
  Quit), then delete this folder and %LOCALAPPDATA%\FitSuite.

More detail: docs\MEASUREMENTS.md (how everything is measured) and docs\MILESTONES.md
(what has and has not been verified). Third-party licences: THIRD_PARTY_NOTICES.txt.
VALORANT is a trademark of Riot Games. FitSuite is not affiliated with Riot Games, Intel,
Corsair or the Raw Accel project.
