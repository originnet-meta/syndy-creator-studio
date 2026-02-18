# Scene 3D Source D3D11 Visual Validation Checkpoints

This checklist is for validating color-space and alpha behavior after IMP-06.

## Preconditions

1. Renderer is set to Direct3D 11.
2. `vspace-source` plugin is loaded.
3. Test scene contains:
   - One `Scene 3D Source` with a model that uses baseColor texture.
   - One `Image Source` with the same reference texture.
   - One `Color Source` background (neutral gray).

## Test Assets

1. Model A: opaque baseColor texture with neutral gradient ramp.
2. Model B: transparent edge texture (soft alpha feather).
3. Model C: high-saturation chart (red/green/blue patches).

## Checkpoints

1. BaseColor sampling path:
   - Model A colors should match the `Image Source` reference within normal visual tolerance.
   - No obvious gamma lift/crush when toggling source visibility.

2. Framebuffer sRGB path:
   - Color should remain stable after switching scenes multiple times.
   - No sudden brightness shift after opening/closing source properties.

3. Premultiplied alpha blending:
   - Model B edge should not show dark halo on gray background.
   - Layer ordering with other semi-transparent sources should look consistent.

4. D3D11-only smoke:
   - Launch OBS, switch scenes 20+ times, reload source settings repeatedly.
   - Confirm no render-thread warnings/crashes related to `vspace-source`.

## Logging Checkpoints

1. Confirm model payload load success log appears for valid models.
2. Confirm no repeated color-space related warning spam during steady rendering.

## Pass Criteria

1. All checkpoints above pass on D3D11 without regressions in scene composition.
2. No visible gamma mismatch against equivalent `Image Source` texture.
3. No alpha fringe artifacts on transparent texture cases.
