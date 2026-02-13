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

# Startup Splash Packaging Validation

## Build and Package
- Preset: `windows-x64`
- Configuration: `RelWithDebInfo`
- Package command: `cmake --build --preset windows-x64 --target package --config RelWithDebInfo`
- Package artifact:
  - `build_x64/obs-studio-32.1.0-windows-x64.zip`
  - `build_x64/obs-studio-32.1.0-windows-x64.zip.sha256`

## Resource and Packaging Inspection
- QRC inclusion check:
  - `frontend/forms/obs.qrc` contains `images/obs.png` (`:/res/images/obs.png`)
  - `frontend/cmake/ui-qt.cmake` includes `forms/obs.qrc` in `target_sources(obs-studio ...)`
- Package extraction path:
  - `build_x64/package_verify_1`
- Binary/resource checks:
  - `bin/64bit/obs64.exe` exists in package
  - Embedded resource marker `:/res/images/obs.png` found in packaged `obs64.exe`
  - Extracted locale file includes startup splash keys:
    - `data/obs-studio/locale/en-US.ini`
    - `Startup.Splash.Status.StartingApplication`
    - `Startup.Splash.ModuleLabel`
    - `Startup.Splash.Reason.Unknown`

## Package Runtime Verification Matrix
| Scenario | Command | Expected | Result |
|---|---|---|---|
| Package basic startup | `obs64.exe --portable --multi --minimize-to-tray` | App starts and initializes in packaged environment. | PASS (`2026-02-13 13-56-01.txt`) |
| Package safe mode | `obs64.exe --portable --multi --minimize-to-tray --safe-mode` | Safe mode path selected. | PASS (`2026-02-13 13-56-09.txt`, `Safe Mode enabled.`) |
| Package only bundled plugins | `obs64.exe --portable --multi --minimize-to-tray --only-bundled-plugins` | Third-party plugins disabled path selected. | PASS (`2026-02-13 13-56-18.txt`, `Third-party plugins disabled.`) |
| Package splash CLI disable | `obs64.exe --portable --multi --minimize-to-tray --disable-startup-splash` | Startup runs with splash disabled option accepted. | PASS (`2026-02-13 13-56-26.txt`) |
| Package splash config disable | `global.ini: [General] EnableStartupSplash=false` + `obs64.exe --portable --multi --minimize-to-tray` | Startup runs with config-based splash disable. | PASS (`2026-02-13 13-57-24.txt`) |

## Missing Resource Check
- Checked packaged run logs for splash resource-related errors:
  - `QPixmap`
  - `obs.png`
  - `resource`
  - `No such file`
- Result: no splash resource missing indicators were observed in package logs.

## Conclusion
- Startup splash packaging path is valid.
- No additional packaging-path or qrc-path code changes were required for IMP-07.
