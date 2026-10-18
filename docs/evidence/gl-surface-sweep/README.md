# REAL surface A/B sweep — scored

Run: 2026-08-24, `CoDSP.exe +spmap chateau`, flat (VR off), 13 stages,
1440×1440 framebuffer captures + per-draw CSV census.
Raw artefacts preserved in `runs/sweep-real/`.

## Verdict

**REAL_SURFACE_ISOLATION = PASS** for the three primary surfaces
(gun, trigger hand, support hand).

**Sleeves: FAIL as a left/right pair** — the split is real but not anatomical.
Detail below; it does not block the native-hands work, and it confirms rather
than contradicts the plan to suppress sleeves.

---

## 1. Per-draw matrix evidence (exact)

Every census row carries both the modelview the ENGINE set (`mv*`) and the one
the draw actually rendered under (`us*`). This does not depend on a screenshot
and cannot be confused by idle animation.

| stage | class moved | applied delta, EYE space | rotation residual | untouched draws with `mv != us` |
|---|---|---|---|---|
| gun_x | gun ×5 | t = (+3.93700, 0, 0) | 2.2e-16 | 0 of 118 |
| gun_yaw | gun ×5 | pure rotation, 15° | — | 0 of 118 |
| handR_x | hand_R ×1 | t = (+3.93700, 0, 0) | 1.7e-18 | 0 of 112 |
| handL_x | hand_L ×1 | t = (−3.93700, 0, 0) | 1.1e-16 | 0 of 147 |
| sleeveR_x | sleeve_R ×1 | t = (+3.93700, 0, 0) | 1.1e-16 | 0 of 147 |
| sleeveL_x | sleeve_L ×1 | t = (−3.93700, 0, 0) | 1.4e-18 | 0 of 147 |

3.937 units = 10 cm at the established 39.37 u/m. **Every draw the probe did
not claim rendered under the engine's own matrix, byte for byte.**

The live split is per-frame and verified: the merged 7824-index hand batch
became `hand_L` (3912 indices, verts 0–836) + `hand_R` (3912 indices, verts
837–1673) drawn from a second index buffer, and the 948-index sleeve batch
became two 474-index halves over verts 0–119 and 120–239. 120 splits verified,
0 rejected, 0 pair mismatches, across 13 stages.

## 2. Framebuffer evidence

Scored by three-frame differencing: every probe stage sits between two
baselines captured with the transform OFF, so

```
signal = |P−A| > T  and  |P−B| > T,     T = max(1.6·|A−B|dilated, 10)
```

`T` is a *per-pixel* floor measured on the two bracketing baselines, not a
global threshold — the frame contains running soldiers and a fading title card
alongside a quiet region that includes the whole viewmodel, and any single
threshold is either deaf in one or blind in the other.

**Null control.** The same detector, given three frames that ALL have the
transform off, in the same A-P-B arrangement:

| triple | signal | largest blob |
|---|---|---|
| base0·base1·base2 | 0.359 % | 5164 px |
| base1·base2·base3 | 0.482 % | 8772 px |
| base2·base3·base4 | 0.128 % | 1729 px |
| base3·base4·base5 | 0.295 % | 403 px |
| base4·base5·base6 | 0.148 % | 495 px |

That is the detector's own false-positive rate on this footage: up to 0.48 %.

**Probe stages.**

| stage | signal | vs worst null | where it moved |
|---|---|---|---|
| gun_x | 9.004 % | 19× | gun body, right half of frame |
| gun_yaw | 6.379 % | 13× | gun body, swept |
| handR_x | 0.279 % | 0.6× | foregrip hand only |
| handL_x | 2.935 % | 6× | lower hand only |
| sleeveR_x | 3.855 % | 8× | sleeve only |
| sleeveL_x | 6.179 % | 13× | sleeve only |

`handR_x` does not clear the null floor **as a percentage**, and that number
alone would have been ambiguous. It is not ambiguous spatially: its three
blobs sit on the foregrip hand (vacated at x≈723–775, arrived at x≈941–1053,
y≈852–965), which is a small, half-occluded surface. See
`handR_x_overlay.png`. The null blobs sit on soldiers.

**Cross-stage overlap** — the isolation claim in one number:

| pair | intersection | IoU |
|---|---|---|
| **handR_x ∣ handL_x** | **0 px** | **0.0000** |
| handR_x ∣ sleeveR_x | 2 px | 0.0000 |
| handR_x ∣ sleeveL_x | 34 px | 0.0003 |
| gun_x ∣ handL_x | 419 px | 0.0017 |
| gun_x ∣ sleeveR_x | 787 px | 0.0030 |
| gun_x ∣ sleeveL_x | 471 px | 0.0015 |
| sleeveR_x ∣ sleeveL_x | 57060 px | 0.3779 |

**The two hands share zero pixels.** That is the result the whole milestone
was for.

`gun_x ∣ handR_x` is 2579 px, which is 45 % of handR's small mask — but that
is occlusion, not coupling: the foregrip hand sits ON the barrel, so moving
the gun changes what covers those pixels. `gun_x_overlay.png` shows the
foregrip hand still in place, gripping air.

---

## 3. Findings that revise the plan

### 3.1 Hand laterality is INVERTED relative to the asset

The classifier assigned sides from `viewmodel_hands_new4`'s declared surface
order (0 = left, 1 = right). What actually rendered:

| index half | rendered position | anatomical role |
|---|---|---|
| verts [0, 837) | bottom of screen, on the stock | **trigger hand — the RIGHT** |
| verts [837, 1674) | up on the barrel | **support hand — the LEFT** |

The asset label and the rendered side genuinely disagree. Fixed in
`MergedHalves()`; pinned by `S_GLSurfLaterality`. Wiring a controller to the
old mapping would have driven the wrong hand.

### 3.2 The sleeve split is not a left/right pair

Only one sleeve is on screen at all — the support arm's. The 240-vertex sleeve
mesh bisects into two halves of the SAME arm, which is why the two sleeve
stages disturb regions overlapping by 71 % of the smaller mask while the hands
overlap by zero. `GLSurf_SleeveSplitIsAnatomical()` returns 0. Sleeves should
be suppressed when the hands go on controllers.

### 3.3 The viewmodel is drawn in TWO different spaces

| draws | modelview | vertices are in |
|---|---|---|
| gun ×3 (`glDrawArrays`, counts 1428/558/750) | axis swap **+ translation (0.763, −4.651, −10.614)** | model space |
| gun ×2 (counts 36/324), hands, sleeves | axis swap, translation ≈ **(1e-5, 0, −2e-5)** | **camera-relative CoD world space** |

For the second family the authored pose is baked into the vertices and is
simply not in the matrix. Two consequences:

- `Delta = Desired · inverse(Authored)` cannot read `Authored` from the
  modelview for four of the seven viewmodel draws.
- A rotation needs a pivot from the geometry. Added:
  `GLSurf_ClassPivot()` (eye-space centroid of sampled vertices) and
  `GLSurf_BuildDeltaAboutPivot()`.

### 3.4 gun_yaw rotated about the CAMERA

The recorded delta for gun_yaw is a pure rotation about eye-space (0,0,0) —
the gun's origin moved from z −10.613 to −10.448, i.e. it swung around the
player's head on a 10-unit arm rather than turning in place. It still proves
isolation (only gun draws changed) but it is not the experiment it looked
like. Fixed via the pivot above; the mutant test measures the old form
throwing the surface 28.3 units across the view.

---

## 4. The 146004688 px reading

Diagnosed and fixed. The metric projected the batch's **matrix origin**. For
the camera-relative family that origin is the eye itself: clip w = −z_eye ≈
2e-5, which cleared the 1e-6 guard and was divided by.

Replaced with a measurement on **real sampled vertices** — each projected
through both matrices, samples not in front of the near plane rejected rather
than divided by, reported as median / p95 / max with valid and rejected
counts. On the exact matrices that produced 146004688 it now reports 163 px.

The retired diagnostic is kept as `GLSurf_TestOriginShiftX` purely so the
suite can demonstrate it still returning 1.27e+08 px on those matrices.

The sweep's per-stage counters were also cumulative, so a baseline stage that
transforms nothing reported "150 transformed draws" inherited from the probe
before it. Stages now report their own deltas.

---

## 5. Files

| file | what |
|---|---|
| `shot_metrics.json` | all numbers above, machine-readable |
| `<stage>_overlay.png` | probe frame, signal tinted, blob boxes |
| `<stage>_triptych.png` | baseline / probe / baseline, cropped |
| `null_<base>_overlay.png` | the detector's false-positive footprint |
| `mask_<stage>.npy`, `field_<stage>.npy` | masks and per-block shift fields |

Regenerate:

```bash
python tools/score_shots.py runs/sweep-real docs/evidence/gl-surface-sweep
```

## 6. What the screenshots do NOT settle

Displacement **magnitude**. The transform is rigid in eye space, but
projection turns that into a pixel displacement scaling as 1/depth, and these
surfaces span roughly 6–60 units of depth; a single 2-D translation does not
fit any of them, and the per-block field is too noisy against moving NPCs to
be load-bearing. Magnitude is settled by the in-probe geometry metric (§4),
which has real vertices in hand. The screenshots settle **where** things
moved, which is what the isolation question actually asks.
