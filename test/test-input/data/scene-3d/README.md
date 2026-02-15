<!--
/******************************************************************************
    Modifications Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>
    Modified: 2026-02-15
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/
-->

# Scene 3D Draco Test Fixtures

This folder contains deterministic fixtures for `scene-3d-draco-loader-test`.

## Files

- `triangle.bin`
  - Shared binary payload with one triangle:
    - POSITION: 3 vertices
    - TEXCOORD_0: 3 UV pairs
    - INDICES: 0, 1, 2
- `accessor-only.gltf`
  - Baseline accessor decode path (no Draco extension).
- `draco-fallback.gltf`
  - Primitive includes `KHR_draco_mesh_compression` plus accessor attributes.
  - Current implementation (decoder unavailable) must succeed via accessor fallback.
- `draco-requires-decoder.gltf`
  - Primitive includes `KHR_draco_mesh_compression` without accessor attributes.
  - Current implementation must fail with `draco_decoder_unavailable`.

## Validation Points

- Smoke:
  - Draco extension present + accessor fallback must stay loadable.
- Regression:
  - Accessor-only path must remain unaffected.
  - Draco-disabled option must still load extension-annotated payload via accessor path.
- Guard:
  - Extension-only primitive must fail deterministically without a decoder.
