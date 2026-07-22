# TonemapMaster — Unified Tonemapper System

TonemapMaster replaces Unreal Engine 5's filmic tonemapper with a **unified
tonemapper system** offering two selectable algorithms:

- **AgX** — Troy Sobotka's AgX display transform (the plugin's original mode).
- **GT7 Color Volume Mapping** — Polyphony Digital's Gran Turismo 7 tone mapper
  (SIGGRAPH 2025), with ICtCp chroma preservation.

Both modes are injected through the same engine-official extension point and
share the same LUT-based rendering pipeline. No engine source or engine shader
files are modified.

---

## 1. Architecture

### Injection point

The plugin registers a `SceneViewExtension`
(`FTonemapMasterSceneViewExtension::SubscribeToPostProcessingPass`) at
**`EPostProcessingPass::ReplacingTonemapper`** — the engine's official hook that
skips its own tonemap pass entirely. The subscription is active only when
`r.TonemapMaster.Enable = 1` **and** the output device is SDR; HDR outputs
(ST2084 / scRGB) fall through to the engine's ACES path.

### Two-pass structure

**Pass A — 3D grading LUT builder** (`TonemapMasterCombineLUTs.usf`, compute
shader)

- A port of the engine's `PostProcessCombineLUTs`, compiled from the plugin's
  own `/Plugin/TonemapMaster` virtual shader directory.
- Bakes white balance, color grading, ExpandGamut, legacy LUT, the **selected
  tone curve** (AgX or GT7), and output-device EOTF encoding into a single
  32³ volume LUT.
- The GT7 branch runs at the exact `FilmToneMap()` call site inside the LUT
  builder, so the surrounding grading math is identical for both modes.
- **Caching:** every input that affects the LUT (grading settings, mode,
  AgX look/contrast mode, all GT7 parameters) is collected into a hash key;
  the LUT is rebuilt only when the hash changes. Otherwise the previous
  frame's LUT is reused at zero cost.

**Pass B — per-pixel tonemap** (`TonemapMasterPS.usf`)

- A port of the engine's `PostProcessTonemap.usf`: scene color + bloom
  composite → eye-adaptation exposure → vignette → 3D LUT sample (LinToLog
  shaper + scale/offset) → dithering.

Because both tone curves are baked into the LUT, **per-pixel runtime cost is
identical for AgX and GT7** — GT7's PQ/ICtCp math runs only during LUT
(re)builds, i.e. 32,768 texels instead of millions of pixels.

### Mode selection

`r.TonemapMaster.Mode` selects the curve evaluated inside Pass A:

| Value | Tonemapper |
|-------|------------|
| `0`   | **AgX** (default) |
| `1`   | **GT7 Color Volume Mapping** |

The master switch remains `r.TonemapMaster.Enable` (`0` restores the engine
filmic tonemapper). The mode is part of the LUT hash key, so switching modes
triggers an automatic LUT rebuild.

---

## 2. Quick start

1. Enable the **TonemapMaster** plugin (Edit → Plugins → Rendering) and restart
   the editor.
2. Confirm the replacement is active: `r.TonemapMaster.Enable 1` (default).
3. Pick a tonemapper:
   - **Editor panel:** open the *Tonemap Master* panel (LevelEditor main menu →
     Mazeline → tonemap master) and use the **Tonemapper** combo (AgX / GT7).
     GT7 parameter spin boxes appear alongside the existing AgX controls.
   - **Console:** `r.TonemapMaster.Mode 1` for GT7, `0` for AgX.
4. Tune parameters either in the panel or via the CVars below — the panel reads
   and writes CVars directly, so changes apply immediately and stay in sync.

---

## 3. CVar reference

### Master / mode

| CVar | Values | Default | Description |
|------|--------|---------|-------------|
| `r.TonemapMaster.Enable` | 0 / 1 | 1 | 0 = engine filmic tonemapper (subscription off) |
| `r.TonemapMaster.Mode` | 0 / 1 | 0 | 0 = AgX, 1 = GT7 Color Volume Mapping |

### AgX

| CVar | Values | Default | Description |
|------|--------|---------|-------------|
| `r.TonemapMaster.Look` | 0 / 1 / 2 | 0 | 0 = Base/Default, 1 = Golden, 2 = Punchy |
| `r.TonemapMaster.ContrastMode` | 0 / 1 | 0 | 0 = reference 4096-entry LUT (bit-exact vs OCIO), 1 = polynomial approximation |

### GT7 Color Volume Mapping

Defaults are Polyphony Digital's reference values.

| CVar | Range | Default | Description |
|------|-------|---------|-------------|
| `r.TonemapMaster.GT7.TargetLuminance` | 250–10000 | 1000 | HDR peak luminance in nits. On SDR output the target resolves to the 100-nit reference, so this has **no visible effect on SDR** (reserved for future HDR support). |
| `r.TonemapMaster.GT7.BlendRatio` | 0–1 | 0.6 | 0 = per-channel curve only; 1 = full ICtCp chroma preservation. |
| `r.TonemapMaster.GT7.CurveMidPoint` | 0.1–1.0 | 0.538 | Toe ↔ linear transition point. |
| `r.TonemapMaster.GT7.CurveLinearSection` | 0.1–2.0 | 0.444 | Linear ↔ shoulder boundary (fraction of peak). |
| `r.TonemapMaster.GT7.CurveToeStrength` | 0.5–3.0 | 1.28 | Toe gamma. Values > 1 darken shadows/midtones (~20% vs filmic at default); move closer to 1.0 to match filmic brightness. |
| `r.TonemapMaster.GT7.CurveAlpha` | 0.01–1.5 | 0.25 | Shoulder convergence rate. Lower = harder clip; higher = softer roll-off. |
| `r.TonemapMaster.GT7.ChromaFadeStart` | 0–2 | 0.98 | Chroma desaturation fade start (relative to target I in ICtCp). |
| `r.TonemapMaster.GT7.ChromaFadeEnd` | 0–2 | 1.16 | Chroma fully desaturated beyond this point. |

---

## 4. GT7 parameter guide

Per-channel tone mappers (including the engine's ACES filmic curve) apply an
S-curve independently to R, G, and B. As channels saturate at different rates
under strong lighting, their ratios break and hues drift — orange pushes toward
yellow, saturated blue toward purple. This **hue twisting** is acceptable in
photorealistic film content but destructive for content where brand or
signature colors are identity (character costumes, stylized art direction).

GT7 Color Volume Mapping attacks this by separating luminance from chrominance
in the perceptually uniform **ICtCp** space (ITU-T T.302, Rec.2020/D65). The
tone curve is still applied per-channel in RGB (preserving the familiar
toe/linear/shoulder response), but the final pixel takes its **luminance (I)
from the curved result** and its **chroma (Ct, Cp) from the original color**.
Near peak luminance the chroma is faded smoothly to zero
(`ChromaFadeStart` / `ChromaFadeEnd`), so only extreme highlights roll off to
white while mid-range colors keep their hue. `BlendRatio` interpolates between
the raw per-channel result (0) and the full chroma-preserving result (1).

Practical tuning notes:

- **Brightness:** the default `CurveToeStrength = 1.28` darkens shadows and
  midtones by roughly 20% compared to the filmic tonemapper, and can make Auto
  Exposure feel less responsive. Set it closer to 1.0 for filmic-like brightness.
- **Shoulder:** `CurveAlpha = 0.25` (default) is a fairly hard shoulder; raise it
  for a gentler highlight roll-off, lower it toward a hard clip.
- **Chroma:** widen the `ChromaFadeStart → ChromaFadeEnd` window to keep
  saturation deeper into the highlights; narrow it for earlier, cleaner
  desaturation of extreme lights.

---

## 5. Integration notes

- **Call site & color spaces.** GT7 runs at the engine's `FilmToneMap()` call
  site inside the LUT builder: AP1 in, AP1 out. Because ICtCp is defined for
  Rec.2020/D65 only, the wrapper converts **AP1 ↔ Rec.2020 with D60 ↔ D65
  chromatic adaptation** (CAT matrices from the engine's `ACES.ush`).
  AgX, by contrast, uses primary-only AP1 ↔ sRGB matrices *without* CAT to
  avoid baking a white-point shift into its nonlinear curve.
- **Chroma preservation.** The tone curve applies per-channel in RGB; luminance
  (I) is then taken from the curved result while chroma (Ct, Cp) comes from the
  **original** color, faded to zero near peak luminance via
  `ChromaFadeStart` / `ChromaFadeEnd`. This avoids ACES-style hue twisting
  under strong lighting.
- **preExposure fixed at 1.0.** Inside the LUT builder, GT7's `preExposure`
  parameter is fixed at 1.0. The LUT must remain a pure color transform;
  exposure is applied outside the LUT by the pipeline. Using `View.PreExposure`
  inside would distort the curve every frame and invert the
  auto-exposure/light-brightness relationship (brighter light → darker image).
- **SDR resolves to the 100-nit reference.** On SDR output the GT7 target
  luminance resolves to the 100-nit reference, so `peakIntensity = 1.0` in
  frame-buffer space and the curve maps naturally into [0, 1]. The reference
  implementation's SDR path with its 0.4 `sdrCorrectionFactor` dimmed the whole
  image and is **not used**.
- **LUT-based cost.** GT7's PQ (ST-2084) and ICtCp math runs only during LUT
  (re)builds — 32³ = 32,768 texels — so runtime per-pixel cost is identical to
  AgX. With the LUT hash cache, frames without parameter changes pay nothing.
- **Provenance.** The GT7 core is an HLSL port of Polyphony Digital's
  MIT-licensed `gt7_tone_mapping.cpp` (SIGGRAPH 2025 Shading Course, *"Driving
  Toward Reality"*). Copyright (c) 2025 Polyphony Digital Inc. Attribution is
  included in `Shaders/GT7Tonemapper.ush` and must be preserved.

---

## 6. Limitations

- **SDR only.** The plugin subscribes at `ReplacingTonemapper` only for SDR
  outputs. HDR outputs (ST2084, scRGB) fall through to the engine's ACES path.
  `r.TonemapMaster.GT7.TargetLuminance` is therefore currently a no-op and is
  reserved for future HDR support.
- **Global CVars, not per-volume.** All settings are global console variables
  (mirrored by the editor panel). PostProcess Volume integration (per-volume
  overrides and blending) is **not included** in the plugin build.
- **LUT resolution.** Both modes share the engine-standard 32³ grading LUT;
  extremely steep curve regions are subject to the same interpolation limits
  as the stock engine path.

---

## 7. License & attribution

- **GT7 tone mapping algorithm:** Copyright (c) 2025 Polyphony Digital Inc.,
  released under the MIT License. Source:
  [gt7_tone_mapping.cpp](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp),
  SIGGRAPH 2025 Shading Course — *Driving Toward Reality*
  ([course page](https://blog.selfshadow.com/publications/s2025-shading-course/)).
  The MIT license notice ships with the shader port in
  `Shaders/GT7Tonemapper.ush`; retain it in any redistribution.
- **AgX:** based on Troy Sobotka's AgX display transform and the OCIO reference
  contrast LUT (`AgX_Default_Contrast.spi1d`).
- **TonemapMaster plugin:** © Mazeline — [mazeline.tech](https://mazeline.tech/).
