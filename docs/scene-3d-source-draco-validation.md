<!--
/******************************************************************************
    Modifications Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>
    Modified: 2026-02-15
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/
-->

# Scene 3D Source Draco Validation Matrix

This document defines IMP-08 validation scope for `scene-3d-source` with a Draco-first focus.

## 1) Scenario Classes

| Class | ID | Goal | Execution |
|---|---|---|---|
| Smoke | `DRACO-SMOKE-01` | Ensure startup decode path is not broken by Draco extension metadata. | Automated (`scene-3d-draco-loader-test`) |
| Regression | `DRACO-REG-01` | Ensure accessor-only models continue to decode. | Automated (`scene-3d-draco-loader-test`) |
| Regression | `DRACO-REG-02` | Ensure `draco_enabled=false` still loads extension-tagged primitive through accessor fallback. | Automated (`scene-3d-draco-loader-test`) |
| Guard | `DRACO-GUARD-01` | Ensure extension-only primitive fails deterministically when decoder is unavailable. | Automated (`scene-3d-draco-loader-test`) |
| Performance | `DRACO-PERF-01` | Measure hitch behavior during repeated large-model reloads (load/update loop). | Manual benchmark pass (not CI-gated) |

## 2) Test-Input Based Automated Validation Points

Fixtures are located in `test/test-input/data/scene-3d/`.

| Fixture | Covered scenario | Expected result |
|---|---|---|
| `draco-fallback.gltf` | `DRACO-SMOKE-01`, `DRACO-REG-02` | Load succeeds, `used_draco_extension=true`, `decode_path=ACCESSOR` |
| `accessor-only.gltf` | `DRACO-REG-01` | Load succeeds, `used_draco_extension=false`, `decode_path=ACCESSOR` |
| `draco-requires-decoder.gltf` | `DRACO-GUARD-01` | Load fails with `draco_decoder_unavailable` |

Harness entry point:
- Target: `scene-3d-draco-loader-test`
- Source: `test/win/scene-3d-draco-loader-test.cpp`
- Build wiring: `test/win/CMakeLists.txt`

## 3) D3D11 Manual Visual Checklist (Draco-focused)

Preconditions:
1. Renderer set to D3D11.
2. `scene-3d-source` plugin loaded.
3. At least one Draco-extension model and one accessor-only model available.

Checklist:
1. Load accessor-only model, confirm stable first render and no warning spam.
2. Load Draco-extension model with fallback-capable data, confirm visible render and no crash.
3. Toggle source visibility and switch scenes 20+ times, confirm no render thread warning burst.
4. Re-open source properties, change model path repeatedly (A/B/A), confirm final model state is deterministic.
5. Confirm log includes fallback warning only when extension is present, not for accessor-only assets.

## 4) CI Minimal Execution Set

Recommended minimum CI set for Windows D3D11 branch protection:
1. Configure/build test target:
   - `cmake --preset windows-x64 -DBUILD_TESTS=ON`
   - `cmake --build --preset windows-x64 --target scene-3d-draco-loader-test`
2. Execute harness:
   - `build_x64/libobs/RelWithDebInfo/scene-3d-draco-loader-test.exe`
   - If needed, prepend runtime deps to `PATH`:
     - `build_x64/deps/w32-pthreads/RelWithDebInfo`
     - `.deps/obs-deps-2025-08-23-x64/bin`
3. Keep `DRACO-PERF-01` outside mandatory CI gate:
   - Run in periodic/nightly pipeline with fixed GPU/driver baseline.

## 5) Notes

- Current implementation does not include a production Draco decoder backend.
- Automated coverage in this phase validates fallback correctness and deterministic failure signaling.
