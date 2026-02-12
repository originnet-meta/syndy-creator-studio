<!--
/******************************************************************************
    Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
-->

# Startup Splash Manual Validation Matrix

## Environment
- Date: 2026-02-12
- Build: `windows-x64` preset, `Debug` configuration
- Binary: `build_x64/rundir/Debug/bin/64bit/obs64.exe`
- Log directory: `build_x64/rundir/Debug/config/obs-studio/logs`

## Scenario Matrix
| Scenario | Launch Command | Expected Result | Recorded Result |
|---|---|---|---|
| Basic startup | `obs64.exe --portable --multi --minimize-to-tray` | Startup path completes without fatal errors. | PASS (`2026-02-12 20-58-15.txt`) |
| Safe mode startup | `obs64.exe --portable --multi --minimize-to-tray --safe-mode` | Safe mode path is selected and third-party risk paths are constrained. | PASS (`2026-02-12 20-58-24.txt`, contains `Safe Mode enabled.`) |
| Only bundled plugins | `obs64.exe --portable --multi --minimize-to-tray --only-bundled-plugins` | Third-party plugins are disabled. | PASS (`2026-02-12 20-58-32.txt`, contains `Third-party plugins disabled.`) |
| Tray start option | `obs64.exe --portable --multi --minimize-to-tray` | Startup command-line includes tray option and startup proceeds. | PASS (`2026-02-12 20-58-41.txt`) |

## Verification Notes
- Each scenario was launched once and allowed to initialize for ~8 seconds.
- Command line verification was done from each run log via `Command Line Arguments:` entries.
- This matrix verifies startup path behavior and option routing; visual UI/splash rendering still requires interactive desktop observation.
