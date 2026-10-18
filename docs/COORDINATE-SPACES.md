# Coordinate spaces for native VR objects

Everything Phases 5–13 do is "put this rendered surface where that controller
is". This is the chain that makes that sentence mean something. No anonymous
axis swaps: every conversion below is named, and the two that are easy to get
backwards are called out.

Worldscale is the established **39.37 units/metre** (1 unit = 1 inch), verified
by the per-eye asymmetric-projection work.

---

## The spaces

| name | units | axes | who owns it |
|---|---|---|---|
| `PoseXRRoom` | metres | +X right, +Y up, −Z forward | OpenXR LOCAL space |
| `PoseHeadLocal` | metres | as above, origin at the HMD | derived |
| `PoseGameWorld` | CoD units | +X forward, +Y left, +Z up | the engine |
| `PoseView` | CoD units | camera basis rows: fwd / left / up | vr.cpp's latched pair base |
| `PoseGLEye` | CoD units | +X right, +Y up, −Z forward | the GL modelview |

## The two conversions worth writing down

### CoD world → GL eye

Measured, not assumed. The viewmodel's modelview is (to 4 decimals) the pure
axis swap

```
eye_x = −world_y      (right   = −left)
eye_y = +world_z      (up      =  up)
eye_z = −world_x      (backward= −forward)
```

which is exactly the matrix the sweep recorded for the hands, the sleeves and
two of the five gun batches — with a translation of ~1e-5, i.e. none.

### XR room → GL eye

XR LOCAL and GL eye share handedness and axis roles, so once the head pose is
divided out the only work is scale:

```
p_eye = 39.37 · Rᵀ_head · (p_room − p_headRoom)
```

`R_head` from the latched HMD orientation. Metres in, CoD units out.

---

## The stereo rule

The GL modelview for a viewmodel draw already contains **that eye's** view
transform, so "eye space" is a different space for eye 0 and eye 1. They differ
by the interpupillary offset. The consequence splits in two, and the halves
behave differently — this is the part that is easy to get backwards, and the
project has already paid once for getting a stereo question wrong.

**Translation is eye-invariant.** The two eye bases are *parallel* — same
orientation, offset laterally — so the same numeric translation in each eye's
coordinates is the same displacement in the world. A pair-latched translation
may be handed to both eyes unchanged. This is why the sweep's `+3.937` on eye X
worked and produced no stereo artefact.

**A pivot is not eye-invariant.** A *point's* coordinates differ between the
eyes by that same offset. Reusing one numeric pivot for both eyes would rotate
the object about two different world points, giving it disparity that
corresponds to no depth — the same class of error as sampling a moving camera
between the eyes.

**An absolute target position is not eye-invariant either.** "Put the hand at
head-relative `p`" becomes `p − eyeOffset_i` in eye *i*'s coordinates. Handing
both eyes the same numeric target places the object at two different world
points.

### What follows

- `vr_objects` latches **rotation and offset only** — both eye-invariant — and
  deliberately has no field for a pivot.
- The pivot comes from `GLSurf_ClassPivot()`, the eye-space centroid of the
  vertices under the engine's own per-eye modelview. It is therefore correct
  for whichever eye is being drawn, automatically, with no eye bookkeeping.
- Any future absolute-placement path (Phase 5's "hand follows the controller
  1:1") **must** subtract that eye's offset. Expressing the desired pose as a
  *delta from the authored pose* avoids the issue entirely, because a delta is
  a translation, and translations are eye-invariant. Prefer the delta form.

---

## Why `Delta = Desired · inverse(Authored)` needs care here

The brief's formulation assumes the authored pose is readable. For three of
the five gun batches it is: their modelview carries a real model→eye matrix
whose translation column is the gun's origin, measured at eye
`(0.763, −4.651, −10.614)`.

For the other four viewmodel draws — the hands, the sleeves, and two gun
batches — it is not. Those arrive **already skinned into camera-relative CoD
world coordinates**; their modelview is the bare axis swap and their authored
position lives in the vertex data. `inverse(Authored)` cannot be formed from
the matrix, and reading the matrix origin anyway is precisely the bug that
reported a 10 cm move as 146004688 px.

So the authored *position* of any viewmodel surface comes from
`GLSurf_ClassPivot()`, which samples the vertices. The authored *orientation*
is not currently recoverable for the camera-relative family and is an open
item — see below.

---

## Open items before Phase 7 can be built

1. **Authored orientation of the camera-relative surfaces.** A position
   centroid is enough to rotate about, but not enough to compute
   "orientation delta from authored to desired". Options: derive a frame from
   the vertex distribution (principal axes), or take the orientation from the
   family-B gun matrix and assume the hands follow the weapon. Neither is
   measured yet.

2. **Weapon profiles.** The rear grip point, foregrip segment and barrel axis
   that Phases 7–9 solve against live in `vr_weapon_profile_table.inc`
   (48 weapons, 29 ADS sight offsets) — in the **retired**
   `COD-VR-offline-hardening` tree, not the canonical one. That tree is not an
   ancestor of the canonical tree, so this is a deliberate file-by-file port,
   not a merge. See `SOURCE-OF-TRUTH.md`.

3. **Sleeves.** Measured non-anatomical: the two halves are pieces of one arm,
   overlapping 71 % on screen. Suppress rather than attach.
   `GLSurf_SleeveSplitIsAnatomical()` returns 0.

4. **Hand laterality is inverted relative to the asset.** Corrected in the
   classifier and pinned by tests; any new code must use
   `GLSC_HAND_R` = trigger hand = index half [0, 837).
