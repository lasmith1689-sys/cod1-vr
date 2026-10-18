# Native hands — what is solved, what is built, what needs the headset

Canonical tree: `H:\Projects\COD-VR-render-hands`
Written 2026-08-25.

## 1. The orientation blocker is solved, and it did not need the DObj

The previous report said camera-relative hand draws expose no useful authored
orientation through their GL modelview. That is correct, and it is not a gap
in the interception — it is what a Quake3-era engine does. **The hands are
skinned on the CPU.** The engine walks the bone hierarchy, blends each vertex
by its weights, and hands the driver a flat array of already-posed positions.
The matrix that accompanies them is the view transform. There is no per-bone
orientation to read because the pose is in the vertices.

Skinning is not lossy for a vertex bound to ONE bone at full weight:

    submittedPos = palmTransform * bindLocalPos

exactly, on every frame of every animation. So the palm's full rigid transform
is **recovered** — closed form, orthogonal Procrustes — from the array the
engine already submits. Not fitted to a shape. Solved for a transform that
provably exists.

The shipped `viewmodel_hands_new` rig has **65 vertices bound rigidly to one
palm bone and 66 to the other**, spread over about 3 x 3 x 1.4 cm. No finger
bone influences any of them, so finger animation cannot flip the recovered
frame — which is precisely the failure mode that rules out a PCA basis.

### The proof

`tools/verify_palm_solve.py` poses the merged hands+gun rig through every
animation each weapon uses, and compares the recovered transform against the
one the engine's own skeleton evaluation produces. Three independent shipped
assets are involved — the surface file supplies the landmarks, the skeleton
file and the animation file supply the truth — so agreement is evidence, not
tautology.

Across **all 28 carried weapons** (`docs/PALM-SOLVE-VERIFICATION.md`):

| | |
|---|---|
| worst rotation error | **0.0001 deg** |
| worst origin error | **0.0002 mm** |
| worst solve-vs-authored per-frame step | **0.000002 deg** |
| flips | **none** |

The last figure is the flip guard and it is deliberately *relative*. The
panzerfaust's fire animation really does rotate the authored palm 97 degrees
in one frame as the tube is discarded; an absolute threshold reported that as
a flip. What matters is that the recovered basis turns by the same amount the
authored one does.

### Route A (read-only DObj) was not needed

The vertex route is exact, needs no syscall, no bone-name resolve, and no
mapping to derive — the recovered pose is already in the space the delta is
applied in. Route A remains available as an independent cross-check if a
future question needs one, but making it the primary path would add a syscall
dependency to something that is currently arithmetic on data the engine hands
over anyway.

## 2. Laterality — corrected, and pinned

    surface 0, vertices [0, 837)     bones `bip01 l *`  -> renders as the RIGHT / trigger hand
    surface 1, vertices [837, 1674)  bones `bip01 r *`  -> renders as the LEFT / support hand

The bone-name prefix and the bone flag byte agree with each other and **both
disagree with what is drawn on screen**. The live attended sweep of 2026-08-25
is the evidence and it wins. Pinned by test fixture: a change back to the
asset labels fails the suite.

Sleeves are NOT an anatomical pair (`GLSurf_SleeveSplitIsAnatomical()` returns
0 — the two halves overlap by 71% of the smaller mask). They are hidden, not
split.

## 3. The gun, by the same mechanism

Gun vertices arrive already posed too, so the pivot has the same problem and
the same answer. Landmarks rigidly bound to the model's body bone give its
transform in view space; the rear grip, expressed in that bone's frame, is
then a real point on the weapon.

**Which model is being drawn is answered by counting the batch's vertices.**
All 24 gun models the game ships have distinct totals — checked, zero
collisions. That is an exact key, not a signature match, and it replaces the
baked-offset identification the profile layer used, which was explicitly a
hypothesis.

- 24/24 models have usable body landmarks (min spread >= 3.2 cm on every axis)
- 18/24 have a STRONG-or-better rear grip; the rest are grenades, the
  panzerfaust and the m1carbine, and their pivots are **refused**, not
  approximated.

## 4. Reload — measured from the animations, not assumed

The offline-hardening tree had no reload data, so it was derived from the
game's own weapon files and reload animations (`tools/extract_reload.py`,
`tools/reload_classify.py`).

The key correction: **the first pass tracked only the support hand**, and so
read every bolt-action rifle as "the hand did not move" and fell through to a
guess. A magazine weapon is reloaded by the support hand; a bolt rifle by the
**trigger** hand, while the support hand cradles the forestock. Which hand
moves is now measured, and is itself the evidence separating a fetched
magazine from a pressed-in clip.

Measured zones, in the weapon's own frame — every one correct against the real
firearm, and none of them derivable from a rule:

| weapon | loading hand | zone |
|---|---|---|
| Thompson | support | z −0.49 — magwell **below** |
| Bren | **trigger** | z +0.21 — magazine on **top** |
| Sten / FG42 | support | y +0.69 / +0.67 — magazine on the **side** |
| Kar98k / Mosin | **trigger** | z +0.09 — charger from **above** |
| M1 Garand | **trigger** | en-bloc clip pressed in |
| Enfield / Springfield | trigger | per-round loop |

A single "magazines go underneath" rule would be wrong for four of these.

18 of 24 models carry a live gesture profile; grenades, turrets and the
panzerfaust correctly carry none.

### One honest residual

`magazineType` mis-labels the Bren as a clip, because the rule "trigger hand
loads it therefore pressed-in clip" cannot see that the Bren's *detachable*
magazine is on top. The reach separates the cases in the shipped data (Bren
0.56 m, Garand 0.43 m, Kar98k 0.21 m) but a threshold picked to reproduce an
answer already believed is not evidence, so none is applied. The field is
marked **WEAK**, and WEAK values cannot become production constants. Nothing
downstream needs it: which hand, which zone, and one-motion-or-loop are all
measured.

## 5. What is in the build

| module | what it does |
|---|---|
| `src/vr_cfg_contract.*` | the production probe contract (section 6) |
| `src/vr_handpose.*` | palm + gun body solve, deltas about the right pivot |
| `src/vr_palm_landmarks.inc` | generated, 48 landmarks per hand |
| `src/vr_gun_landmarks.inc` | generated, 24 gun models + rear grips |
| `src/vr_reload.*` | the physical-reload gesture machine |
| `src/vr_reload_table.inc` | generated, measured zones per gun model |
| `src/vr_weapon_profile.*` | ported, 38 weapon profiles with confidence |
| `src/vr_glsurf.cpp` | `GLSurf_SetHandTarget` — pose-driven hands at draw time |

### The stereo rule, applied

What is latched per XR pair is **eye-invariant by construction**: an offset (a
translation *difference*, identical in both eye bases because they are
parallel) and a rotation. What is *not* latched is the pivot — it comes out of
the palm solve on each eye's own vertex data. The API deliberately offers no
way to latch an absolute position, because that is exactly what would break.

## 6. The production probe contract

Probe cvars are inert unless the config first says `vr_probe_arm 1`, and the
contract is re-asserted after the whole file is parsed so key ordering cannot
defeat it. A missing, unreadable, malformed or unarmed config all end at:

    vr_disable 0    sweep 0    autoquit 0    probe class none

`build.ps1 -Deploy` reads the config it is about to copy and refuses to deploy
an armed one. `tools/flat_run.ps1` now emits the arming line, and restores the
shipped config when the run finishes.

## 7. Hardware-only acceptance list

Everything below needs a headset and cannot be settled offline. Nothing here
is blocked on further engine research.

1. **Controller-to-palm calibration.** `VRHandPose_DesiredFromGrip` takes an
   offset and a rotation; their values are a property of a player's grip on a
   physical controller. Offline they are zero.
2. **Ammo-pouch zone.** Head-relative default is 45 cm down, 15 cm forward,
   22 cm radius. That is a choice about a play space, marked as such.
3. **Zone radii and pull threshold** (12 cm / 15 cm) — comfortable-reach
   numbers, not measurements.
4. **rearGripErrorCm.** The offline solve places the pivot on the rear grip to
   1e-4 m. What a *player* perceives as the gun meeting their hand depends
   on (1).
5. **supportResidualCm** for the two-hand solve.
6. **Comfort of the physical-sights posture** — whether raising the gun to the
   eye reads as aiming.
7. Whether the reload gesture is discoverable without instruction.

## 8. Still to build

- Live controller sampling wired through `vr_objects` into
  `GLSurf_SetHandTarget`. The seam exists and is tested; the caller does not.
- The two-hand solver's support-point sliding along the foregrip interval.
- Fire direction from the solved barrel axis. Deliberately last: it must not
  be wired until the physical gun render is confirmed, and it must never
  reintroduce game-aim to body-yaw to camera feedback.
