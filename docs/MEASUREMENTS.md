# What FitSuite measures, and how

Every number on screen carries one of these labels:

| Label | Meaning |
|---|---|
| **MEASURED** | Recorded on this PC during a FitSuite test or a PresentMon capture. |
| **ESTIMATE** | Derived from measurements by a model (for example the in-sample effect of a correction). |
| **PREFERENCE** | Your own rating or choice. |
| **ENTERED** | Typed in by you (DPI, sensitivity, overdrive setting). |
| **DETECTED** | Read from Windows (CPU, GPU, driver, monitor, refresh rates). |
| **SIMULATED DATA** | Synthetic data from the development harness or tests. Never a measurement. |

When a whole group of numbers shares one origin (for example the tiles of one calibration), the label is
shown once for the group.

---

## MouseFit

### Test scene and mapping

* FPS-style camera (yaw/pitch) with a fixed crosshair. One mouse count turns the view like your main game:
  its published turn rate × sensitivity (VALORANT 0.07°, CS2 and Apex 0.022°, Overwatch 2 and Call of Duty
  0.0066°), or for games without one, the cm/360 you enter. Horizontal field of view 103° at 16:9 (VALORANT's).
* Input comes from Windows Raw Input on a dedicated thread, timestamped with `QueryPerformanceCounter`
  as each report arrives. Only one mouse is used per session (the one you pick, or the first that moves).
  Software-generated input (reports without a device handle, e.g. `SendInput`) and absolute-position
  devices (tablets, Remote Desktop, most VMs) are ignored and counted.
* Raw input is registered only while a test runs, and reports are discarded whenever FitSuite is not the
  foreground window. Leaving the window pauses the test, frees the pointer and discards the interrupted
  target (it is asked again later so blocks stay balanced). Esc stops at once.
* During a test the scene renders without vsync (tearing when Windows allows it) at a capped rate of twice
  the refresh rate (240–1000 fps). Frame pacing and the mouse report rate are recorded with the session,
  and a calibration with uneven rendering is flagged.

### Rounds (FitSuite 0.2 and later)

Every test is played in short rounds, like an aim trainer. A round starts with a click (or Space). From
then on a new target appears **the moment the last one was hit or missed**; there is no waiting or holding
still. One shot per target: a click off the target is a **miss** and the target disappears. Every target
has a **timer** (a ring around it that empties): 2.2 s for flick and micro targets, 3 s for switching
targets; a target that runs out is a timeout. The view is re-centred at the start of every round.

**Calibration**, five rounds (about 4 minutes):

| Round | What | Targets |
|---|---|---|
| 1 Flicks | 8 directions (every 45°) × 4 distances (7°, 13°, 20°, 26° of rotation), radius 1.2° | 32 out from the centre, each followed by one back to it: 64 |
| 2 Micro-adjustments | 8 directions × 2 distances (2°, 4°), radius 0.6° | 32 |
| 3 Returning flicks | as round 1, new order: flick out to a target, then back to the dot in the middle | 64 |
| 4 Tracking | keep the crosshair on a target (radius 1.8°) wandering at 6–18°/s within ±15° × ±8°, easing into a new course every 0.8–1.6 s; a faint trail shows where it was over the last 0.15 s, and aiming at the trail counts as on target | 40 s |
| 5 Target switching | 3 targets at a time at random spots within ±18° × ±9°, radius 1.0°; shoot them in any order | 36 |

The out-and-back pairs keep the view near the horizon (so the direction you move the mouse matches the
direction you see) and give every direction its opposite. The "back" target is placed at the round's
centre, wherever the crosshair ended up. Nerd stats → Mouse → Calibrate can set 1–4 flick rounds and the
flick target size.

FitSuite 0.1 sessions (hold still 250 ms, a random foreperiod, then one target; repeated clicks allowed
until a 3 s timeout) are still read and analysed the same way.

### Segmentation (per trial)

Reports are resampled at 1 ms and speed is smoothed with an 8 ms Gaussian.

* **Movement onset:** walking back from the first speed peak, the point where the speed falls below 5% of the
  peak or stops falling (a speed minimum). The minimum matters in rounds: the hand may still be settling from
  the last target when the next one appears. Onset earlier than 100 ms after the target appeared is treated as
  a guess and the trial is excluded (not in switching rounds, where all targets were already visible).
* **Everything is measured from where the crosshair was at movement onset:** the target's distance and
  direction, and the first flick's endpoint. Leftover movement before onset therefore does not count as part of
  the flick (in simulations, settling movements of up to 0.6° changed the average distance error by 0.2 points
  and the average angle error by less than 0.1°).
* **End of the first flick (primary submovement):** whichever comes first of a direction reversal, the
  speed falling below 10% of peak, a speed minimum under 50% of peak followed within 120 ms by a rise of
  10% of peak (a corrective submovement starting), the first click, or the end of the trial.
* Trials with a peak speed under 30°/s (no real flick) or a gap of more than 60 ms in the report stream
  during the flick (after onset) are excluded.

### Per-trial metrics

| Metric | Definition |
|---|---|
| Reaction time | Target onset → movement onset. |
| Distance error | `(endpoint · target direction) / target distance − 1`, at the end of the first flick. + overshoot, − undershoot. |
| Angle error | Angle from the target direction to the first-flick endpoint, counter-clockwise on screen positive. |
| Endpoint error | Distance from the first-flick endpoint to the target centre, in degrees of view rotation. |
| Correction time | End of the first flick → hit. |
| Acquisition time | Target onset → hit (timeouts count as 3 s). |
| First-click hit | The first click after the flick landed on the target. |

Over/undershoot is measured where the first flick ends, before any correction. Measuring only at the
click would hide the original aiming error.

### Correction model

A correction maps each input vector to `out = g(θ) · R(δ(θ)) · in`, where θ is the movement direction,
`g` a gain and `R(δ)` a rotation by a small angle.

* **Smooth directional (in-app only):** `log g(θ)` and `δ(θ)` are 2nd-order Fourier series (smooth and
  periodic, no seam). The gain series has no constant term, so your average sensitivity is unchanged; the
  angle series may include a constant rotation.
* **Raw Accel-compatible:** the transform Raw Accel 1.7 can apply: rotation, then sensitivity with a Y/X
  ratio, a left multiplier (applied while moving left) and an up multiplier (applied while moving up).
  Together these give separate sensitivities for up, down, left and right, so "flicks toward the bottom of
  the screen land short" becomes "more sensitivity while moving down" and nothing else. FitSuite's
  implementation is a line-by-line transcription of Raw Accel's order of operations, so the in-app validation
  tests exactly what Raw Accel would do. Its overall sensitivity is rescaled so the average gain over all
  directions is 1.
* **Bounds:** gain within ±8% and angle within ±3° in every direction (hard limits 15% and 6°), enforced by
  scaling the whole correction down, never by clipping it. For the Raw Accel model the bound is checked on the
  resulting gain and angle in each direction, because two multipliers can stack (e.g. Y/X and up).
* **Direction estimate in-app:** θ is taken from an exponentially weighted sum of recent input (3 ms
  half-life), so the correction does not jitter with per-report quantization. Zero input always gives zero
  output; the transform never adds movement.

### Fitting

* Uses the scored calibration flicks only. Flicks with a distance error beyond ±60% or an angle error
  beyond 30° are excluded as gross errors.
* Gain: robust (Huber) regression of `log(radial ratio)` on per-distance intercepts (your normal under- or
  overshoot at each distance, deliberately kept) plus the Fourier terms. Angle: robust regression of the
  angle error on a constant plus Fourier terms. The coefficients actually used are lightly shrunk toward
  zero (ridge prior: 0.05 on each log-gain coefficient, 2° on each angle coefficient).
* Each component is tested against "no correction" with a heteroscedasticity-robust Wald test referred to an
  F distribution at α = 0.01. The Raw Accel model is fitted as `endpoint ≈ c_distance · RA⁻¹(target)` (the
  noisy endpoint on the left, which avoids errors-in-variables bias) and tested with a trial-clustered
  robust Wald test.
* A component is kept only if it is significant **and** at least 1% in gain or 0.5° in angle somewhere.
  Otherwise the advice is to keep your current setup.
* The "before → after" bias numbers on the Candidates tab are **in-sample estimates** from the same flicks
  (assuming your hand would move the same way). Only the blinded comparison shows whether a correction
  helps.

These procedures are checked by simulation in the unit tests (simulated players, not people):

| Situation | Result |
|---|---|
| Player with no directional bias, round-based calibration | correction proposed in 3 of 60 calibrations |
| 10% vertical undershoot, round-based calibration | found (vertical boost proposed) in 12 of 12 |
| Player with no directional bias, FitSuite 0.1 calibration (4 blocks) | correction proposed in 4 of 120 calibrations |
| Candidate identical to the baseline, 8-block comparison in rounds | "improved" in 1 of 200, "worse" in 7 of 200 |
| Identical candidate, noisy player with strong drift during the session | "improved" in 2 of 120 |
| 10% vertical undershoot, exact correction, 8-block comparison | "improved" in 24 of 30 |
| Unbiased player, candidate with 15% too much vertical sensitivity | recommended in 0 of 20 |
| Downward flicks 12% short, everything else fine (6-block calibration) | Raw Accel candidate: down +6.3%, up −1.6% relative to average (capped by the 8% bound) |

So a standard comparison rarely recommends a useless correction, and it misses a real 10% correction
about one time in five; the longer comparison lengths reduce misses.

Distance levels: the fit gives each planned distance its own average under/overshoot, separately per kind of
round (a 4° micro-adjustment is not the same movement as a 4° flick); switching flicks are grouped into
distance bands (under 5°, 5–10°, 10–16°, over 16°). A level with fewer than 5 usable flicks is left out.

### Aim report (after a calibration)

For each kind of round: targets hit out of targets shown (misses and timeouts count against it), the median
time from a target appearing to the hit, and for switching the hits per minute. These are MEASURED values of
that session, shown for your information; the fix is fitted from the flick, micro and switching flicks only.

**Flick path:** for every first flick (movement onset → end of the first flick), how far the path bows
sideways from the straight line between its start and end, as % of that line's length (to the left or right as
you look along the flick), and its straightness (straight-line distance ÷ path length; 100% = straight). The
report names the direction with the largest average curve when it is at least 3% and at least 2 points more
than the median direction; otherwise it says the flicks are about equally straight. Nerd stats → Mouse lists
both per direction. During the test a thin, fading line shows the crosshair's last 0.25 s, so you can see the
shape of each flick. This is for your information; the fix does not use it (it corrects where the flick ends).

**Tracking:** the share of time the crosshair was on the target or its 0.15 s trail, the average distance from its centre, and how
far behind the target you stay in each direction it moves. "Behind" is the distance along the target's path ÷
its speed, as time, taken only while the target has moved steadily for 0.25 s (and not in the first 0.3 s of
the round). Everyone trails by about a reaction time; a direction where you trail at least 30 ms more than your
median is named ("you fall furthest behind targets moving down"). In simulations a player whose downward
movements were 15% weaker trailed downward targets by about 100 ms against about 40 ms upward, and an unbiased
player was reported as keeping up equally in every direction.

### Blinded comparison (validation)

* Blocks alternate between your current setup (A) and the candidate (B) in a counterbalanced order
  (ABBA BAAB …). You are not told which block is which.
* Each block is a round of 8 targets out from the centre, in directions offset by 22.5° (±6° jitter) at 9° or
  20° (±12%, alternating, swapped every block), each followed by one back to the centre: 16 targets, none at
  directions or distances calibration used. A short warm-up round (8 targets, not scored) comes first.
  Standard length: 8 blocks (about 2 minutes); 12 and 16 blocks are available.
* **Aim time** = movement onset → hit (timeouts count in full), on a log scale, adjusted for target
  distance and direction by regression (the layout is balanced between conditions, so this only removes
  noise). Reaction time is excluded because a mouse correction cannot change it. A **miss** (the target
  disappeared) has no aim time; it counts against the first-click hit rate.
* Uncertainty: two-stage bootstrap (resample blocks, then trials within blocks; 2000 repetitions), 95%
  percentile intervals. Block-to-block drift therefore widens the interval instead of looking like an effect.
* **Recommended** only if all of these hold:
  * aim time is shorter with 95% confidence (upper end of the interval below 0%);
  * first-click hit rate is not shown to be lower, and its estimate is at most 5 points lower;
  * first-flick endpoint error is not shown to be larger, and its estimate is at most 10% larger;
  * the candidate was faster in at least 75% of block pairs.
* **Worse** if aim time is longer with 95% confidence, or accuracy is shown to be lower.
* Otherwise **no reliable difference**, and the advice is to keep your baseline. The default answer is
  always the baseline.

### Quick check (does the fix still fit?)

Aim drifts: a new mouse pad, a new grip or simply weeks of play can change how flicks land. The quick check
answers one question with about a minute of flicks: is there a directional bias left **with the fix in use**?

* **Targets:** 3 rounds of 8 targets out from the centre (one per direction, at 10° or 22° of rotation,
  alternating and swapped every round) and 8 back to it: 48 targets, radius 1.2°, 2.2 s each. The fix is in use for every target: FitSuite applies it in the test scene (Raw Accel is
  on your own settings while FitSuite is in front), or Raw Accel already applies it when you made the fix your
  Raw Accel default.
* **Analysis:** the flicks are measured in output space (the crosshair movement with the fix applied) and
  fitted with the calibration model above, at α = 0.005 instead of 0.01 because a short session gives the tests
  less data, with at least 4 usable flicks per direction and 32 in all (otherwise: "not enough clean flicks to
  tell").
* **Expected leftover:** a fix capped at its bounds (an 8% limit against a 14% bias, say) leaves part of the bias
  by design. FitSuite replays the fix's own calibration flicks through the fix and fits them again (bounds up to
  the hard limits, lenient α = 0.2, no size gates), and the check compares your new flicks with that expected
  leftover instead of with zero.
* **Verdict:** **changed** only if a component is significant **and** its fitted size reaches 5% of distance or
  2.5° of angle in some direction (2.5° is below the 3° bound, so a rotation can still be seen). The explanation
  names the direction ("your down flicks land 9% shorter than your typical flick"). Otherwise **still fits**.
* The check never changes the fix. A new fix always comes from a full measurement and a blind test.
* **Reminders:** every 7 days by default (30, or never, in Settings). Shown in the Mouse tab's box 5 and, while
  FitSuite waits in the notification area, as a Windows notification at most once a day and never while the fix
  is on in a game.

Checked by simulation in the unit tests (`tests/test_check.cpp`, simulated players):

| Situation | Result |
|---|---|
| Player with an 8% vertical undershoot, unchanged since the fix was measured | "changed" in 6 of 150 checks |
| Precise player with a 14% undershoot, fix capped at the bounds, unchanged | "changed" in 4 of 120 (104 of 120 when compared with zero instead of the expected leftover) |
| Unbiased player without a fix | "changed" in 1 of 150 |
| The vertical undershoot gone, downward flicks now 12% short | noticed in 85 of 120 |
| The vertical undershoot doubled | noticed in 54 of 60 |

---

## FrameFit

### Source

Intel PresentMon (console app, MIT licence) records Windows graphics events (ETW). It does not inject into
the game. FitSuite starts it with:

```
PresentMon --process_name VALORANT-Win64-Shipping.exe --output_file <csv> --timed <seconds>
           --terminate_after_timed --no_console_stats --stop_existing_session --session_name FitSuite
           [--hotkey F10 | --delay 10]
```

PresentMon needs administrator rights or membership of the "Performance Log Users" group. FitSuite only
asks Windows to elevate PresentMon when you tick the box; FitSuite itself never runs elevated. CSV files
from PresentMon 1.x, 2.x and the current default column set are all accepted (also by import); the column
each metric was read from is recorded with the run.

### Per-run metrics

| Metric | Definition |
|---|---|
| Frame time | Time between consecutive presents (`MsBetweenPresents`, or `FrameTime` in 2.x captures), ms. |
| Average FPS | Number of frames ÷ total duration (not the average of per-frame FPS). |
| 1% / 0.1% low FPS | 1000 ÷ mean of the slowest 1% / 0.1% of frame times. |
| P99 frame time | 99th percentile of frame times (linear interpolation). |
| Stutter | A frame at least 2× the median frame time of the previous 0.5 s **and** at least 4 ms longer than it. Reported per minute. |
| Latency | Only when PresentMon reports it: frame start → display (2.x `DisplayLatency`) or present → display (`MsUntilDisplayed`). The definition is stored with the run. |

Frame time is not mouse-to-screen latency. Neither latency column includes the mouse, USB polling, the
game's input sampling before the frame starts, or the monitor's pixel response. End-to-end latency needs
an external sensor and is not measured.

### Comparing configurations

* You change game settings yourself (VALORANT has no supported settings interface) and tick "I have set these
  in VALORANT". Runs record that confirmation. FitSuite never claims to have changed a game setting.
* Each configuration's runs are averaged; the difference to the reference gets a 95% Welch interval.
* A difference is called **better/worse** only when the interval excludes zero **and** it exceeds a practical
  threshold: 3% for average FPS, 5% for 1% lows, P99 frame time and latency, 1 per minute for stutters.
  Otherwise it is **within noise**.
* Comparisons are flagged **noisy** when run-to-run variation is high (coefficient of variation above 3%
  for average FPS or 8% for 1% lows) and **preliminary** with fewer than 3 runs per configuration; neither
  produces a recommendation.
* The recommendation follows the goal you pick (consistency = lowest P99 frame time, throughput = highest
  average FPS, latency = lowest measured latency) and never accepts a candidate that makes frame-time
  consistency worse.

---

## MotionFit

Software cannot observe a panel's pixel response from rendered frames, so every MotionFit result is what
**you** could see, labelled as observed results or preferences.

### Identification test

* A ring with a gap (Landolt C) moves horizontally across the screen for 450 ms after a 600 ms fixation
  dot; you follow it with your eyes and report the gap direction (4 choices; guessing is 25% correct).
* Speeds are in screen widths per second: 0.25, 0.35, 0.5, 0.7, 1.0, 1.4 and 2.0 (×√2 steps; 2.0 is the
  fastest whose path still fits on screen). Each speed appears twice per block, and each setting gets 3
  blocks (42 answers per setting). Settings alternate block by block.
* The test draws exactly one new position per refresh (vsync). Refreshes that repeated an old frame are
  counted from DXGI frame statistics; affected trials are reported.
* **Refresh-rate comparisons are blinded:** FitSuite switches the rate between blocks without saying which is
  active (a temporary change that Windows undoes if FitSuite exits), waits 1.8 s for the monitor to settle,
  and restores your setting afterwards.
* **Overdrive comparisons are not blinded:** overdrive has no standard DDC/CI control, so you switch it in the
  monitor menu when asked, and you know which setting is active. Results are labelled accordingly.
* Optionally the moving target updates only at the frame rate FrameFit measured (your baseline 1% low), so the
  comparison reflects what you see in game without variable refresh.

### Analysis

* Psychometric function: `P(correct) = 0.25 + 0.75 · (1 − 0.02) · logistic(−(log2 v − μ) / s)`, fitted by
  maximum likelihood per setting.
* **Readable speed** = the speed at which 75% of answers are correct, with a 95% bootstrap interval. If it falls
  outside the tested speeds it is shown as "< 0.25" or "> 2.0" rather than extrapolated.
* Two settings are compared by the ratio of their readable speeds (bootstrap interval). **Better** requires the
  interval to lie above 1× and the interval of the overall-accuracy difference to stay above −2 points.

### Trail inspection

Moving blocks cross grey lanes; you rate dark smearing (ghosting) and bright/dark halos (overshoot) from
0 (none) to 4 (very visible) for each setting, at least twice. These are **preferences**, shown as averages.

### Applying a mouse correction (Raw Accel)

* **Create a Raw Accel settings file** writes `settings.fitsuite-<time>.json` next to your Raw Accel
  `settings.json`: your profile with the correction combined into its rotation and ratios (your Output DPI is
  scaled, not replaced). Your `settings.json` is not changed.
* **Apply now** runs Raw Accel's own `writer.exe` with that file. Raw Accel then adjusts all mouse movement on
  the PC by direction, until Windows restarts or Raw Accel loads `settings.json` again (Raw Accel's FAQ: "The
  driver itself does not store your settings"; they have to be applied again after each start). FitSuite treats a non-zero
  exit code as a failure; the writer itself reports problems in its own message window.
* **Make it my default** first copies `settings.json` to `settings.before-fitsuite-<time>.json`, then puts the
  corrected settings in its place and applies them.
* **Restore** puts the saved copy back and applies it. Every step is listed under History → Changes made
  by FitSuite.
* The correction depends only on the direction you move. Nothing reads the screen, the game or enemy positions.

### Using the fix only in your games (per-game switching)

The Mouse tab's box "4 Use it in your games" switches the fix on only while a game you ticked is in front:

* **What is read.** While this mode is on, Windows tells FitSuite when a different window comes to the front
  (`SetWinEventHook` for `EVENT_SYSTEM_FOREGROUND`, out of context: the event is posted to FitSuite's own thread
  and nothing is loaded into other programs). FitSuite then asks which window is in front
  (`GetForegroundWindow`) and which process owns it; as a fallback it also asks every 2 seconds while hidden and
  up to 10 times a second while its window is open. The executable name (for example
  `valorant-win64-shipping.exe`) comes from a system process snapshot (`CreateToolhelp32Snapshot`), taken only
  when a different program comes to the front. FitSuite never opens the game's process, and never reads its
  memory, its screen, its input or anything that happens in a match. Without this mode there is no hook.
* **The decision.** The fix is wanted when the program in front is on your list. A change is made only after the
  new state has held for 1 second, so a quick Alt-Tab does not flip settings back and forth. Two cases are
  immediate: the first decision after FitSuite starts, and switching off when FitSuite itself comes to the front,
  so its own tests always measure your settings without the fix (a test does not start until Raw Accel has
  confirmed the switch). Unit tests cover these rules (`tests/test_game_activation.cpp`).
* **The switch.** "On" runs Raw Accel's `writer.exe` with `rawaccel-with-fix.json` in FitSuite's data folder:
  your current `settings.json` with the chosen fix combined into its rotation and ratios, made again before every
  switch so later changes in Raw Accel or a newly chosen fix are picked up. "Off" runs `writer.exe` with your own
  `settings.json`, which this mode never changes. Raw Accel itself accepts at most one change per second (its
  README: a one-second delay "to mitigate its abuse"), so the fix is active a second or two after the game comes
  to the front. Settings change only when the program in front changes, never in response to anything in a game.
* **Problems.** If the writer reports an error, or stays open for more than 10 seconds (it is probably showing a
  message), switching pauses and the box says why; nothing is retried until you click **Try again**.
* **Running in the background.** While the mode is on, closing the FitSuite window hides it in the notification
  area (right-click the icon to quit). While hidden FitSuite draws nothing, releases its Direct3D device, asks
  Windows for efficiency mode (EcoQoS, `ProcessPowerThrottling`) and below-normal priority, trims its working
  set, and sleeps until a window message, a program switch or its next due work. `writer.exe` is started at
  normal priority. The 1 ms timer resolution is requested only while a test runs.
* **Startup entry.** **Settings → Open FitSuite when Windows starts** (off until you tick it; also offered in box
  4) writes one per-user value: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `FitSuite`,
  command `"<path>\FitSuite.exe" --autostart` (refreshed at each start if FitSuite was moved). Started that way,
  FitSuite stays in the notification area unless **Start in the notification area** is unticked, and quits at
  once (removing the value) if the setting is off. Unticking removes the value; **Turn off** for the per-game
  fix does not. Task Manager → Startup apps can switch the entry off without removing it
  (`…\Explorer\StartupApproved\Run`); FitSuite reports that in Settings and Check my setup and only switches it
  back on if you tick the box again. The entry is listed under History with a **remove** link.
* **Quitting, shutting down, restarting.** When FitSuite quits, and when Windows shuts down or signs you out
  (`WM_ENDSESSION`), it puts Raw Accel back on your own `settings.json` and saves. Because Raw Accel's driver
  keeps no settings over a restart, FitSuite's first decision after it starts is immediate: your own
  `settings.json`, or the fix if one of your games is already in front.
* **Sensitivity.** The fix is a relative change applied to mouse counts before the game sees them, so it rides on
  top of each game's own sensitivity: at most 8% more or less in any direction and unchanged on average. It was
  measured with your VALORANT sensitivity, so it carries over to games where you use about the same distance for a
  full turn (cm/360).
* **Game sensitivity (Settings).** Full-turn distance = `360 / (yaw × sensitivity × DPI)` inches × 2.54, with each
  game's published turn rate per mouse count at sensitivity 1 (yaw): VALORANT 0.07°, Counter-Strike 2 and Apex
  Legends 0.022°, Overwatch 2 and Call of Duty 0.0066°. Other games scale sensitivity with their field of view or
  their own formula, so you enter the distance yourself (ENTERED). A game within 10% of your VALORANT distance is
  "about the same"; more than 15% faster or slower is named on the Mouse tab, because the fix may fit it less
  well.

### Refresh-rate check

For every display attached to the desktop FitSuite reads the current refresh rate and the rates Windows offers at
the current resolution and colour depth (`EnumDisplaySettingsEx`, read-only; the monitor itself is not asked).
If the current rate is at least 5 Hz below the fastest one (so 59 vs 60 Hz does not count), the Mouse and Monitor
tabs show a warning and Check my setup says "Check". The list is read at start, when Windows reports a display
change (`WM_DISPLAYCHANGE`) and when Check my setup runs. **Keep … Hz** stores the monitor (EDID id, else its
name) with that fastest rate, so a newly offered faster rate warns again. Unit-tested in `tests/test_motion.cpp`.

### Installing Raw Accel from the setup

Only when you click **Download and install Raw Accel**: FitSuite downloads the official release zip
(`RawAccel_v1.7.1.zip` from github.com/RawAccelOfficial/rawaccel) over HTTPS and uses it only if its SHA-256 is
exactly `770fe3ae0919ca3c4d412f58c985eb27f5434decad809f7e8206de4e8852eec4` (the published 1.7.1 zip). It unpacks
it with Windows' own `tar.exe` to `%LOCALAPPDATA%\Programs\RawAccel` and starts Raw Accel's `installer.exe`
with administrator rights, which Windows asks you for. Raw Accel's guide says a restart finishes the install;
after it, FitSuite asks you to open Raw Accel once and click Apply so it writes its `settings.json`. FitSuite
also checks for the Visual C++ runtime Raw Accel needs and links to Microsoft's download if it is missing.
Existing Raw Accel folders (with `writer.exe`) are looked for in Downloads, Desktop, Documents, Program Files
and C:\, one or two levels deep, by folder names containing "rawaccel".

### Updates

When **Check for updates once a day** is on (Settings → Updates; the setup asks), FitSuite reads
`https://api.github.com/repos/tsbforlife123/FitSuite/releases/latest` once a day, sending only the request
itself (its user agent is `FitSuite/<version>`). If the release is newer, the status bar and Settings say so;
nothing is downloaded until you click **Download the update**. FitSuite then downloads `FitSuite.exe.sha256`
and `FitSuite.exe` from that release and uses the program only if its SHA-256 matches. **Update now** renames
the running `FitSuite.exe` to `FitSuite.exe.old`, puts the new one in its place, starts it and quits; the new
version deletes the old file. If any step fails, the original file is put back. Your data folder is not
touched. The checksum protects against a damaged or swapped download, not against someone who controls the
GitHub account that publishes releases; code signing would add that (FitSuite is not signed yet).

### Applying a refresh rate

"Switch Windows to … Hz" changes the saved display mode and asks you to keep it within 15 seconds; otherwise it
reverts automatically, like Windows' own display settings. Every change is listed under History →
Changes made by FitSuite with the way back.
