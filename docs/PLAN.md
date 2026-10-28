# FitSuite — technical plan and feasibility

FitSuite is the working name of the desktop app; MouseFit, FrameFit and MotionFit are its three tools.
First target game: VALORANT. Everything runs locally: no account, cloud service or AI API.

## 1. Starting point

The working directory was empty (no repository, no existing user work to preserve).
The user's own PC was not reachable from this session, so nothing here has run on Windows
hardware yet. See `MILESTONES.md` for what was actually executed.

## 2. Stack

| Layer | Choice | Why |
|---|---|---|
| Language | C++20 | Direct access to every Windows API we need; one toolchain for app, math core and tests. |
| Window, input, display | Win32 (Raw Input, SetupAPI/HID, QueryDisplayConfig, EnumDisplaySettings, ChangeDisplaySettingsEx, dxva2 DDC/CI) | These are the supported interfaces; no wrappers needed. |
| Rendering | Direct3D 11 flip-model swap chain | Mature, low overhead, tearing support (`DXGI_FEATURE_PRESENT_ALLOW_TEARING`) and vsync control for frame-paced tests. |
| UI | Dear ImGui 1.93.0 + ImPlot 1.1 (both MIT) | One render loop for the app shell *and* the test scenes, so frame pacing stays under our control; strong graphing; small static binary. |
| Data | nlohmann/json 3.12 (MIT), versioned JSON files | Human-readable profiles, easy validation. |
| Tests | doctest 2.5 (MIT) + synthetic/recorded input | The math core has no OS headers and is tested on any platform. |
| Build | CMake; clang/lld with llvm-mingw (UCRT) or MSVC | Produces a single `FitSuite.exe`; no runtime installers. |

Rejected: WPF/WinUI (weak control over frame pacing, needs D3D interop for test scenes),
Electron (large, still needs native modules for raw input, PresentMon and DDC/CI),
game engines (heavy, less control over the input path).

## 3. MouseFit

### 3.1 What PureVector does (public information)

Corsair's PureVector (with Aimlabs, SABRE v3 PRO PureVector mice): a questionnaire plus five one-minute
SpiderShot rounds in Aimlabs, a report of aim bias in eight directions and the share of perfect, under- and
over-flicks, and calibrated settings written to the mouse's onboard firmware for one DPI stage. Nothing runs
during play. Corsair does not publish the correction model.

MouseFit uses the same idea (direction-dependent correction from measured flick bias) with its own
measurement environment, a published and bounded model, held-out blinded validation, and an explicit choice
of output path. It does not require Corsair hardware.

### 3.2 Three separate capabilities

| Capability | Status |
|---|---|
| **Read input** from a specific mouse | Supported now: Raw Input (`WM_INPUT`) on a dedicated thread with QPC timestamps; device path, VID/PID and HID product string; software-injected input (`hDevice == NULL`) is rejected; absolute-mode devices (tablets, RDP, VMs) are refused. |
| **Create and test** a directional profile | Supported now, entirely inside FitSuite. |
| **Apply** the profile outside FitSuite | Partially possible, only through Raw Accel and only for a constrained profile family (rotation plus separate up/down/left/right sensitivity; below). FitSuite applies it with Raw Accel's own `writer.exe` when the player clicks, with backup and restore. Not verified in VALORANT by us. |

### 3.3 System-wide output path (investigated first)

| Path | Can it represent our function? | Verdict |
|---|---|---|
| **Raw Accel 1.7** (MIT, signed kernel filter driver acting on the raw input stream). Pipeline read from its source: rotate → angle snap → acceleration → × sensitivity, × Y/X on Y → × L/R if x < 0 → × U/D if y < 0. | Only four shape parameters: constant rotation, Y/X ratio, left multiplier, up multiplier. Its anisotropic "range/domain" weights act only through speed-dependent acceleration curves. It cannot represent a smooth per-direction gain/angle function. | **Usable for an approximation.** MouseFit fits a Raw Accel-compatible profile and validates *that exact transform* in-app. Raw Accel's FAQ states it works in VALORANT; we have not verified this. Needs admin install + reboot; settings must be re-applied after each boot (GUI or `writer.exe`). |
| Mouse vendor software / firmware | Some mice support separate X/Y DPI (equivalent to a Y/X ratio) or "angle tuning" (sensor rotation). Directional corrections exist only on PureVector-capable Corsair mice via Aimlabs, with no public API. | Hardware-dependent; manual entry only. We do not write mouse firmware. |
| Interception driver | Could carry any function by intercepting and re-injecting input. | **Rejected.** Riot stated in 2020 that VALORANT will not launch while Interception is loaded. |
| Synthetic input (`SendInput`) | — | **Rejected** by design; unreliable for games and ignored by raw-input readers. |
| Custom KMDF mouse filter driver implementing the exact function | Yes. | **Requires research:** Microsoft attestation signing (EV certificate), kernel-mode QA, anti-cheat acceptance. Not started. |
| VALORANT settings | One sensitivity + scoped multiplier, no X/Y split. | Not a path. |

**Consequence for the design.** The calibration engine is built around a general smooth directional
model, and a second, constrained model whose math is a transcription of Raw Accel's pipeline. The two are
fitted from the same calibration data and validated separately. The full smooth model is labelled
"in-app only" everywhere; only the Raw Accel-compatible model is offered for export.

### 3.4 Measurement and model (summary — full definitions in `MEASUREMENTS.md`)

* FPS-style yaw/pitch camera with a stationary crosshair, VALORANT mapping (0.07° per count × sensitivity),
  103° horizontal FOV at 16:9.
* Targets in 8 directions × 3 distances; the scheduler keeps the camera near the horizon so the
  count-space direction matches what the player sees.
* Each trial is segmented: reaction → primary submovement (the first flick) → corrections → hit.
  Overshoot/undershoot and angular error are measured at the *end of the first flick*, not at the click.
* Profile = `out = A(θ̂)·in`, with `A(θ) = g(θ)·R(δ(θ))`; `g` and `δ` are 2nd-order Fourier series
  (periodic, smooth, no seam at ±180°). The mean gain is fixed at 1, so overall sensitivity never changes.
  Gain deviation is capped at ±8% (hard limit 15%) and angle at ±3° (hard limit 6°) by scaling, not clipping.
  `θ̂` is the direction of a short time-weighted sum of recent input, so per-report quantization does not
  jitter the correction. Zero input always gives zero output; the transform never adds motion.
* Corrections are only proposed when the directional terms are statistically distinguishable from zero.
* Validation: counterbalanced, blinded blocks (ABBA BAAB), fresh target directions/distances, two-stage
  bootstrap, block-pair consistency, non-inferiority on accuracy. Default recommendation: keep baseline.

## 4. FrameFit

* **Measurement foundation:** Intel PresentMon console app (MIT). FitSuite launches it with
  `--process_name VALORANT-Win64-Shipping.exe --output_file … --timed N --terminate_after_timed
  --no_console_stats --stop_existing_session --session_name FitSuite`, or imports CSVs captured elsewhere.
  PresentMon needs administrator rights or membership in "Performance Log Users"; FitSuite itself does not.
* PresentMon's CSV columns differ between 1.x, 2.x and the current default; the parser maps all three and
  records which column each metric came from.
* Frame time is not mouse-to-photon latency. FitSuite reports frame-time statistics and, separately and
  only when PresentMon provides them, its display/PC latency columns with their exact definitions.
* VALORANT has no supported settings API, so game settings are changed by the user through a guided
  checklist and recorded as *user-confirmed*, never *applied*.

## 5. MotionFit

* Monitor identification: QueryDisplayConfig / DisplayConfigGetDeviceInfo (friendly name, connector, EDID
  manufacturer and product IDs, exact refresh rate), EnumDisplaySettings (resolution, supported modes).
* Refresh rate can be switched through ChangeDisplaySettingsEx. During tests the switch is temporary
  (`CDS_FULLSCREEN`), so Windows reverts it if FitSuite exits or crashes; this also allows blinded
  refresh-rate comparisons.
* DDC/CI (dxva2) is read-only in this build and only queried when the player asks (it talks to the monitor and can take seconds). MCCS reserves VCP codes E0h–FFh for manufacturers and we found
  no standard overdrive code, so overdrive is changed by the user in the monitor menu.
* Software cannot see the panel's pixel response. All MotionFit results are user-observed
  (identification accuracy, ratings, preferences) and labelled that way. Camera-based measurement is a
  later feature.

## 6. Feasibility table

| Feature | Supported now | Requires research | Hardware-dependent |
|---|---|---|---|
| Raw mouse capture, device identification, injected-input rejection | ✔ | | Polling rate / HID strings vary by mouse |
| Directional calibration, fitting, blinded validation (in-app) | ✔ | | |
| Apply full smooth profile system-wide | | ✔ custom signed driver | |
| Apply Raw Accel-compatible profile | ✔ export of values | ✔ verify in VALORANT | Requires Raw Accel install |
| Write profile to mouse firmware | | | Only specific vendors/mice; not attempted |
| PresentMon capture and CSV import | ✔ | | Some GPU/latency columns depend on driver |
| CPU/GPU busy timing | ✔ when PresentMon reports it | | GPU tracking support varies |
| End-to-end mouse-to-photon latency | | ✔ needs external sensor (e.g. LDAT-class) | |
| Change VALORANT settings automatically | | | Not possible (no supported API); guided manual |
| Switch display refresh rate (with revert) | ✔ | | Supported modes vary |
| Read DDC/CI capabilities | ✔ | | Monitor must support DDC/CI |
| Control overdrive | | ✔ per-model vendor codes | Not standardized |
| Measure ghosting/response time automatically | | ✔ camera/photodiode method | |
| Motion-clarity tests (user-observed) | ✔ | | Results depend on panel |

## 7. Module layout

```
core/      platform-independent library (no OS headers except the atomic-file helper)
  fit/profile, store      shared setup profile, validation, atomic saves, staleness
  fit/mouse_*             transform, segmentation, metrics, fitting, Raw Accel model, schedules, comparison
  fit/frame_*             PresentMon CSV parsing, frame statistics, run comparison
  fit/motion_*            motion-test scheduling and psychometric analysis
app/       UI pages, session flows, test scenes (Dear ImGui), talks to platform services through interfaces
platform/win32/  raw input thread, display/DDC/CI, system info, PresentMon runner, Win32 + D3D11 main
harness/linux/   development harness (X11/GLX) used to render the UI here; not a product build
tests/     unit tests, synthetic user model, recorded-input fixtures
```

## 8. Build order (from the brief) and status

1. Plan + feasibility — this file.
2. Stack — §2.
3. Desktop shell + shared profile model.
4. MouseFit raw input + target test.
5. Directional measurements, fitting, blinded comparison.
6. FrameFit capture and comparison.
7. MotionFit guided tests.
8. Shared profiles across modules.
9. Supported settings application and restoration.
10. Windows package.

Progress against each step is tracked in `MILESTONES.md`.
