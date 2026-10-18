## 2026-09-06 pm2: both hands ride the weapon (trigger hand lock)

User: "right hand should freeze to model as well, in general, including
during aim down sights." `vr_trigger_hand_lock 1` (VR Options, HANDS /
WEAPON): whenever the weapon is native the RIGHT hand's render target is the
authored right palm carried by the weapon's applied rigid transform -- the
same construction as the left hand's support lock, one absolute target per
pair. The controller still drives the weapon through the palm calibration
(the gun anchors on the physical palm target, which is not touched), so aim,
bore and two-hand are byte-identical with the lock on or off (tested); the
drawn hand now follows the gun through two-hand bar alignment, stabilisation,
kick attenuation, the gun trim (vr_gun_yaw 2) and the sighted animation
instead of staying on the controller and opening the grip. One-handed with a
consistent relation the ride is a proven no-op (2e-7). The hand
independence test keeps its lock-off contract and gains the lock-on one.
Gate 2139 / 0 safe, 135 / 0 DLL.

## 2026-09-06 pm: precision batch (candidate 4BB9EF2D, cfg A95FF7C9)

Follows the Codex comfort update (CCB87E6E: measured level hold, floating
hands, support stabilisation, X tap/hold, VR Options growth) and the user's
in-headset saves (r_pitch -23.00, r_yaw -18.00, gun_yaw 2.00, haptics 0.8),
which are now the shipped values. Requests: bullets always down the barrel;
crosshair optional, off; the LEFT hand static on the weapon while supporting
(no clipping); a brightness control that works.

* **No spread, no view kick**: `tools/build_precision_pk3.py` writes
  `dist/zz_codvr_precision.pk3` from the stock pak0 weapon files -- 28
  carried firearms (weaponType bullet, slot primary/pistol), the 8 spread
  and 8 view-kick fields zeroed, every other field byte-identical (checked
  twice, report in docs/precision_weapons.json). Deployed into main\ by
  build.ps1 (loads after the stock paks by name); remove it to restore
  stock spread. Nothing in the exe is touched.
* **Crosshair**: `vr_crosshair` (VR Options, default 0) is mirrored into
  main\autoexec.cfg as `seta cg_drawCrosshair` by the proxy whenever the key
  is applied (load or a menu change), so it takes effect at the NEXT launch.
  Pure helper `src/cfg_line.h`, tested with CRLF/set/seta/duplicate mutants.
* **Support pose lock** (`vr_support_pose_lock 1`): while the two-hand
  support is attached, the LEFT hand's target is the authored left palm
  carried by the weapon's own rigid transform (desired rear grip + basis
  against the last real body solve; authored space is camera-relative, so
  both eyes share ONE target and the stereo verdict holds by construction).
  The hand is drawn exactly where the game's animation holds it on the gun.
  The first attempt borrowed the gun's per-eye delta in vr_glsurf and the
  production soak caught 756 stereo rejections (the carried baseline lags
  one frame differently per eye); the per-pair target has zero.
* **Brightness** (`vr_brightness`, VR Options DISPLAY, shipped 0.85): a
  multiplicative gain (GL_ZERO/GL_SRC_COLOR quad) in the vignette pass on
  the finished eye image; the menu overlay draws after it and stays bright.
  Pixel-tested: 0.5 halves the centre and the corner, twice gives a quarter.
* Snapshots re-enabled at 15 s + trigger, cap 80.

Gate: safe suite 2120 / 0, DLL 135 / 0. Deploy now also carries the pk3 and
the autoexec, with their hashes in deploy-provenance.json.

## 2026-09-06: in-engine snapshots (candidate 566A0465, cfg 56BCAF26)

Request of 2026-09-06: "take snapshots of the game in engine so you can
also see what I'm seeing". `src/vr_snapshot.cpp` reads the FINISHED eye
texture back (glGetTexImage after the vignette and the VR Options overlay,
so it is exactly what the headset shows), box-downsamples it 1/2 and writes
`snapshots\snap_NNN_L.png` / `_R.png` beside the DLL plus `snap_NNN.txt`
with the controller grip/aim, the native-hands targets, the gun target and
bore pitch/yaw of that pair. Sources: an 8 s timer and every RIGHT trigger
press (1.5 s debounce, cap 120 pairs). Pure readback: no probe, capture or
autoquit is armed (deploy contract still reads "unarmed"); GL pack state and
texture binding are restored. Gate: safe suite 2031 / 0 (decision logic,
PNG writer against an independent decoder with a flipped-byte mutant,
downsampler), DLL 135 / 0; PIL decodes the test PNG. Hands/gun code
untouched since A0BB1935.

## 2026-09-04f: the hand "vertically up 90 deg" -- the calibration, once more
### (candidate A0BB1935, cfg 8A2ABA5F)

Brief of 2026-09-04e (hand orientation). Gate: safe suite 2003 / 0, DLL 135 / 0.

**The brief's premise was wrong and the log shows it.** The brief froze the
weapon ("the gun looks correct") and asked for a hand-only rotation. The
2026-09-04e hardware log's aim beats put the gun's BORE pitch at median +51
deg and max +86 deg over the session, against median +1.2 deg on the
previous candidate (75E9A890). Nobody holds a rifle at 85 deg for a session:
the whole hand-plus-gun assembly was pitched ~90 deg up relative to the
controller, and because the aim follows the bore the gun "shot where it
pointed" and the user saw the wrongness in the hand. The hand and gun were
still the authored rigid pair to 0.003 u, so a hand-only rotation would have
detached the hand from the weapon the user is holding. The weapon therefore
DID change in this candidate, deliberately: it is level again, as on
75E9A890.

**ROOT CAUSE: the OpenXR grip convention, mis-stated twice.** The spec
(OpenXR 1.0, "Grip pose") says: +X is the palm normal (into the palm on the
right), **-Z** runs through the closed fingers little finger -> thumb, +Y =
Z x X. So for a hand: thumb side = -Z, palm-out = -X (right), and the WRIST
side is +Y -- the hand points along -Y. Up to 2026-09-04c the generator had
Z's sign wrong (hands backwards/upside down; the 180 deg runtime flip fixed
the orientation but was never applied to the offset -- the "controller in
the weapon body"). On 2026-09-04d I misread the spec as "+Y little->thumb,
+Z fingertips->wrist" and rebuilt the table 90 deg off about the palm
normal -- the bore went up. Now the generator quotes the spec verbatim, the
rows are (t, s*n, -k) and the offset (-C.t, -s*C.n, +C.k), and a test asserts
the WEAPON'S BORE along the controller's -Y (0.997), with BOTH earlier
tables rebuilt as mutants and caught (knuckles at -1.000 and 0.000 of the
pointing axis).

**Section 5 candidates.** The rotation from the 2026-09-04d frame to the
final one is exactly 90.0 deg about the grip's X (the palm normal); every
other quarter-turn leaves >= 90 deg of error. See the report table.

**Section 6, one final transform.** ControllerGrip -> calRows (below) ->
DesiredRightPalm; the weapon hangs off that by the live authored relation;
the rendered hand is that palm turned by `vr_hand_r_visual_pitch/yaw/roll`
(a NEW hand-only trim about the palm pivot, shipped at identity; the
weapon, bore, aim and left hand are byte-identical with it on or off).
Right hand, hands_new4: rows (+0.020 +0.109 -0.994) (-0.249 +0.963 +0.100)
(+0.968 +0.246 +0.047), offset (+0.0132 -0.0094 +0.0497) m.

## 2026-09-04e: palm anchor, real bore, Colt, grenade
### (candidate 0F80B977, cfg 968A1AD4)

Brief of 2026-09-04d, P0-P5. Gate: safe suite 1995 / 0, DLL suite 135 / 0.

**P0 -- the controller did not own the palm. ROOT CAUSE: the calibration was
derived against a mis-stated OpenXR grip convention.** The generator's
docstring had the grip's Y and Z axes swapped ("+Z little finger -> thumb,
+Y along the metacarpals"); the spec says +Y is little finger -> thumb and
+Z runs from the fingertips to the wrist. Measured against the spec, the
shipped rows were exactly 90.0 deg off about the palm normal on BOTH hands,
and the 180 deg runtime flip candidate 52B9D826 carried did NOT reduce that
(still 90.0 deg to spec). A weapon carried rigidly by a palm pitched 90 deg
sweeps its body through where the hand actually is, which is the hardware
report "the controller appears to own a point nearer the centre/butt/body of
the weapon". The calibration is regenerated against the spec, the runtime
flip is REMOVED, and `vr_native_hands_palm_flip` /
`vr_native_hands_gun_follow_fix` are now parsed-but-inert so an older config
still loads and the log says so. There is one intentional calibration and no
patch on top of it. New gate: the same controller pose gives the SAME right
palm on BAR / Thompson / Colt / M1 / MP40 (0.000000 u, 0.000000 deg apart),
and the mutant that anchors to the weapon body is caught.

**P1 -- shots three feet high. ROOT CAUSE: `barrelForward` is the CHORD from
the weapon frame's origin to the muzzle tag, not the bore.** On a rig whose
frame origin sits off the bore that chord tilts; on the Thompson it tilts
6.64 deg ABOVE the sight line, which is 0.914 m (three feet) at 7.9 m --
the hardware report to the metre. The evidence was already in the shipped
profile: `barrelForward.estimates.fromRotation` (the muzzle tag's own
rotation) and `sightForward` agree to 1.16 deg while `fromPosition`
dissents by 6.64. The fire direction is now a derived BORE per model, taken
from the sight line and cross-checked against the muzzle tag's rotation,
carried in the body-bone frame with the muzzle point beside it
(`boreFwd` / `muzzleM`). Corrections applied: Thompson 6.64 deg, Luger
4.58, PPSh 2.97, MP44 2.06, Colt 1.93, everything else under 1.5.
`barrelFwd` stays where it belongs, as the body long axis the two-hand
support slides along.

**P2 -- the Colt still doubled. ROOT CAUSE: the carried pre-key delta.** The
2026-09-04d log shows the Colt drawing `[123:S 270:S 344:F 48:F]` every
eye-scene -- its trigger and slide are ALWAYS drawn before the body keys --
and `staleErrMax 4.541 u / 20.512 deg`: the carried previous-scene delta
absorbs the animation the body's fresh delta cancels, so those parts wobble
20 deg away from the weapon. Suppressing them would have hidden a pistol's
slide permanently, so a pre-key part now keys ITSELF: its landmark table is
expressed in the body frame at rest, so a solve that passes the residual
gate both proves the part is at rest and yields this scene's body pose. A
part mid-animation fails that gate and is hidden for those frames only. A
vertex span that is ambiguous within one weapon (the Enfield ships two
46-vertex clips, which are translations of each other and both solve) may
never key anything -- measured, the wrong choice put the pose 6 u out while
passing every residual gate. The carry is now the opt-in fallback
(`vr_gun_prekey_policy`, default 0).

**P3 -- the grenade floated two feet above the hand. ROOT CAUSE: mixed
ownership by design.** A weapon whose rear grip is below STRONG was refused
outright, which left it head-coupled beside a controller-owned hand. It is
now placed rigidly by the LIVE authored palm->body relation, pivoting on the
body's own origin. Nothing is invented: that relation is measured from the
same two baselines the rifle uses, and a rigid placement does not depend on
which point it is expressed about. In the 10k soak the grenade was equipped
on 1630 pairs and controller-owned on 1607.

**P4 -- two-hand.** The slide point is now chosen by DISTANCE (solve
|A + tB| = |hand - grip| for t, clamped to the legal segment) instead of by
projecting the hand onto the segment: the projection moves when the
alignment rotates the weapon, which left the bar a fraction of a degree
behind for a pair. Measured: 2 mm of left-hand travel moves the weapon
0.370 deg (geometry says 0.377), 5 mm 0.928 (0.944), 10 mm 1.856 (1.887),
each inside ONE XR pair, residual 0.000-0.028 deg, and ten out-and-back
moves leave it where it started to 0.0000 deg.

**P5 -- the left sleeve.** Not rebuilt. The forearm is skinned across five
bones and the anatomical placement was still called disconnected on
hardware, so `vr_sleeve_visible 1` draws the RIGHT sleeve only. The LEFT
HAND is untouched and still controller-owned. `vr_sleeve_visible 2` restores
both when the arm is worth revisiting.

**Soak (10,000 pairs, six weapons including the grenade):** 56 weapon
switches with the controller held still and ZERO palm jumps (worst
0.00000 u), zero owner conflicts, zero mixed-owner eye-scenes, zero bore
mismatches.

## 2026-09-04d: weapon ownership, left sleeve, two-hand, UI safe area, VR options
### (candidate 75E9A890, cfg 7B08F54E)

Brief of 2026-09-04, phases A-E. Gate: safe suite 1981 / 0, DLL suite 135 / 0.

**P0 -- duplicate / jumping non-BAR weapons. ROOT CAUSE: shader-sorted draw
order.** The engine issues a weapon's surfaces sorted by shader, not in asset
order, so for the Colt and the Thompson a NON-BODY surface (slide, trigger,
clip, bolt) reaches GL BEFORE the body surface that keys the scene's one
rigid delta. Those pre-key batches fell through to the engine head-coupled
while the body went to the controller: two weapons on screen, jumping with
the animation. Evidence from the 2026-09-04c hardware log, per weapon key:
BAR (516) 0 vanilla batches in 27,719 eye-scenes; Thompson (326) 0.888 and
Colt (344) 1.037 vanilla batches PER EYE-SCENE (11,250 / 8,712 vanilla
against 39,254 / 24,888 transformed), with ZERO solve refusals -- so the
weapon was identified and driven, and part of it was still drawn vanilla.
That is class B/G of the brief's list, not a stereo scheduler fault (0
half-native pairs, 0 stereo rejects in the same log).

FIX. Every shipped gun surface is bound 100 % rigidly to ONE bone
(measured, tools/extract_gun_surfaces.py), so a per-SURFACE landmark table
in the BODY-bone frame lets ANY body-bound surface key the scene, whichever
the engine draws first: src/vr_gun_surfaces.inc, 105 surfaces over 24
models, 44 of them key-usable (>= 8 landmarks, >= 2 cm spread). A moving
part is listed for identification but NEVER keys -- a part that has moved
with its animation still fits its own table and would report its own pose
as the body's. A gun batch drawn before the key is placed by the carried
previous-scene baseline (`vr_gun_prekey_policy 1`, measured deviation
reported) or suppressed -- never vanilla. Every gun draw is attributed to
exactly one disposition (fresh / stale / suppressed / vanilla), and
native-plus-vanilla in one eye-scene is an OWNER CONFLICT the pre-submit
verdict refuses within the fallback budget (VRNH_VERDICT_OWNER_CONFLICT).

**P1 -- the left sleeve.** The sleeve surface is NOT rigid: 120 vertices
skinned across forearm, upper arm, cuff, wrist and hand, with 8 (right) / 0
(left) vertices fully rigid to the forearm, which is why it could only ever
take the hand's whole delta and swung as a wedge. The forearm's ORIENTATION
is recoverable from the vertices the forearm dominates (weight >= 0.75)
behind a 25 mm gate: ~1-4 deg against the true bone over the shipped idle
and fire animations. The sleeve is now placed by ONE rigid motion about the
anatomical wrist: authored wrist -> desired palm origin (exact, from the
palm solve), drawn forearm orientation -> DesiredPalm x RestRelation, the
forearm continuing the knuckle line with the roll of the idle animations
(src/vr_sleeve_landmarks.inc). Never the sleeve's centroid, never the
camera, never the gun frame. A sleeve whose elbow would reach the eyes is
not drawn (`vr_sleeve_hide_near_head 1`); an unsolvable sleeve falls back to
its hand's delta, still connected. No arm IK.

**P2 -- two-hand support.** Tuning was the problem, not the solver: the
measured grab radii are 7.5-17 cm and the legal slide was +/- 10 cm, so the
2026-09-04c run attached with the slide clamped at t=0 on most beats. Now:
a forgiving radius floor (12 cm), a longer legal slide (+/- 15 cm), release
hysteresis independent of the tight measured radius (25 cm), attach
IMMEDIATELY on a squeezed grip inside the radius (no candidate dwell), and
the orientation blend armed ONLY by attach/release (0.08 s) -- FREE <->
CANDIDATE no longer arms it, so hovering near the fore-end can no longer
low-pass the one-hand weapon. Steady attached control is rigid: 0.0000 deg
bar residual, 0.000 cm support residual, a 10 deg hand step reaches the gun
as 9.81 deg in ONE pair. Telemetry: twohand state / grabDistance /
supportT / residual / angular residual / attach / detach / reason.

**P3 -- VR UI safe area.** One inset on every 2D surface, menus, loading
screens and the gameplay HUD alike, aspect preserved, centred; the world
keeps the full eye and the runtime is never lied to. `vr_ui_scale 0.92`
(with per-axis `vr_ui_safe_x` / `vr_ui_safe_y`): the flat panel's angular
fit goes from +/-33.9 x 26.2 deg to +/-31.7 x 24.3 (tan ratio 0.920 on both
axes), the HUD band from 1008x567 to 927x522 at 1440^2.

**P4 -- VR Options.** A CODVR-owned overlay (src/vr_menu.cpp), not a patch
of the game's menu: injecting an entry would mean editing the protected
executable. Hold the LEFT stick click for 0.6 s; sticks navigate and
adjust, A changes, B saves and closes. 34 settings in five groups, every
value shown, applied immediately through EXACTLY the codvr.cfg path
(dllmain's chain was extracted into ApplyConfigKey so there is no second
copy of the semantics), restart-only settings saved but never applied live,
saved back into codvr.cfg in place with comments and unrelated keys intact.
No probe / diagnostic / experimental key is exposed.

## 2026-09-04c: whole assembly corrected (cfg 4D39719C on DLL 52B9D826)

Hardware feedback on 52B9D826/227C79C1: the hand-only correction (weapon frozen per brief) rotated
the hands away from the rifle. The user's ruling: the authored hand + sleeve + gun assembly was
constructed correctly and only needed turning as ONE unit. Fix = config only:
`vr_native_hands_gun_follow_fix 1`, which anchors the gun on the corrected right-hand basis, so
hand, sleeve and weapon carry the same 180 deg about grip X and keep the authored relation
(S_PalmOrientationFix gunFollow case: relation preserved < 1e-2 u, barrel.knuckles +0.996).
Sleeves already take their hand's rigid delta, so the left limb is unchanged in construction.
The barrel override (physical aim) now follows the corrected gun, i.e. forward with the controller.
DLL byte-identical (52B9D826); cfg 4D39719C; checkpoint
`checkpoints/candidate-52B9D826-2026-09-04-candidate-52B9D826-cfg4D39719C-whole-assembly-2026-09-04`.

# Native-hands campaign -- phase status (2026-09-04, second cut)

## 2026-09-04b: VISUAL LIMB CORRECTION, MEASURED FROM THE RUNTIME GRIP
- Hardware on 3352E385 (logs/hw-2026-09-04b/): limbs still point the wrong
  way. Root cause of the 3352E385 miss: the eyeish grip rows are
  (grip -Z, grip -X, grip +Y); the "180 deg about grip X" was implemented
  as "keep row 0" = about grip FORWARD (the barrel/knuckle axis) -- a roll,
  not the flip. Removed; replaced by ONE correction: keep row 1 (grip -X),
  negate rows 0 and 2 (grip -Z, +Y) = 180 deg about grip-local X.
- GROUND TRUTH, measured: the 3352E385 log's pairs 1-5 carry the RIGHT GRIP
  quaternion (-0.738 -0.287 -0.244 -0.560) with the trigger pulled while the
  user pointed the controller at the start menu. Through the production chain
  (tools/cal_flip_probe.py): grip +Y -> room backward, grip -X -> palm-out,
  grip -Z -> thumb side. Rendered RIGHT anatomy in room (fwd,left,up):
    uncorrected  knuckles (-0.97 -0.15 -0.21) thumb (+0.25 -0.68 -0.69) palm (+0.04 +0.72 -0.70)  backward, thumb down
    3352E385     knuckles (+0.97 ..) thumb (.. -0.69) palm reversed                              rolled
    THIS         knuckles (+0.97 +0.15 +0.21) thumb (-0.25 +0.68 +0.69) palm (+0.04 +0.72 -0.70) forward, thumb up, palm to midline
  LEFT (mirrored grip) identical with the palm to the midline. Relative to the
  uncorrected render: F'.F = -1, L'.L = -1 (thumb), U'.U = +1 (palm normal):
  a 180 deg PROPER rotation about the palm normal, det +1, no mirror. (The
  brief's F'=-F, U'=-U, L'=+L was inferred from the words; the runtime grip
  says the thumb reverses and the palm normal holds.)
- APPLIED to the hand render target ORIENTATION only, about the existing palm
  pivot (position identical to the bit); the sleeve takes the same rigid
  delta (matrix equality asserted). WEAPON FROZEN per brief: the gun anchor
  still reads the UNCORRECTED basis -> PairWorld pos/axis, rear grip and
  barrel forward byte-identical (0.000000).
- FINDING (reported, not acted on): the frozen barrel is OPPOSITE the
  corrected hand's knuckles (dot -0.996). Both hardware logs' aim(beat)
  lines show the barrel-derived fire direction converged 150-180 deg from the
  body facing during play (+167.7, -179.9, -178.8, +172.6, +168.0 deg): a
  gun anchored on the flipped basis renders barrel-backward, mostly behind
  the camera. `vr_native_hands_gun_follow_fix 1` (default 0) anchors the gun
  on the corrected basis: barrel with the knuckles (dot +0.996), authored
  palm->rear-grip relation preserved (< 0.25 mm), hand pivot unmoved.
- Gate: 1769 safe + 131 DLL = 1900 passed, 0 failed. S_PalmOrientationFix
  rewritten around the measured grip (both hands, pivot, sleeve coherence,
  weapon byte-identical, follow-fix rigid pair). Wiring test expectation
  updated to the corrected grip rows.


# Native-hands campaign -- phase status (2026-09-04)

## 2026-09-04: HAND-ORIENTATION FIX (backwards+upside-down) -- candidate 3352E385
- HARDWARE VERDICT on DA3BCE74 (logs/hw-2026-09-04/): flicker FIXED, stereo
  stable, two hands tracked, ownership independent, gun articulation + firing
  direction + two-hand + right-stick + camera all WORKING. Ownership counters
  on hardware: order normal 1786 / swapped 1342 / unknown 0, duplicate 0,
  pairFailClosed 0 -- the surface-order fix held. All these subsystems FROZEN.
- The ONLY failure: both rendered hands backwards and upside down (position ok).
- ROOT CAUSE: the asset-derived ControllerGripToPalm was validated only against
  an ASSUMED OpenXR grip basis; the real runtime grip basis differs by a fixed
  180 deg about the grip-local X axis. Measured offline (tools/cal_flip_probe.py)
  against the shipped rig: the current rendered palm has knuckles pointing at
  the user and palm-out reversed, thumb side correct -- exactly a 180 deg about
  the palm thumb/lateral axis == 180 deg about grip-local X (equivalent axis
  computed as (1,0,0) for BOTH hands, det +1, no mirror).
- FIX: apply the 180 deg grip-local-X flip to the HAND RENDER TARGET ORIENTATION
  ONLY (FlipGripRowsX180 before the calibration in VRNH_OnPairBeginEyes).
  Position is taken from the uncorrected solve (translations preserved). The
  value the GUN anchor reads (VRNHHandDebug::targetAxisEyeish) is left
  uncorrected, so gun body / rear grip / barrel / gameplay aim are BYTE-IDENTICAL
  (proven: S_PalmOrientationFix runs a pair with the flip off vs on and asserts
  gun PairWorld pos/axis delta 0.000000). Toggle vr_native_hands_palm_flip
  (default 1). Effective corrected ControllerGripToPalm:
    RIGHT rows (-0.0204 -0.1087 -0.9939)(+0.2491 -0.9633 +0.1002)(-0.9683 -0.2456 +0.0468)
    LEFT  rows (-0.0205 +0.1026 -0.9945)(+0.2366 +0.9669 +0.0949)(+0.9714 -0.2334 -0.0442)
  translations unchanged (RIGHT off -0.0132/-0.0094/-0.0497 m, LEFT -0.0139/+0.0101/-0.0490 m).
- Gate 2026-09-04: **1766 safe + 131 DLL = 1897 passed, 0 failed** (adds
  S_PalmOrientationFix: 180 deg proper rotation both hands, gun byte-identical,
  knuckles+palm reverse, thumb preserved). All frozen subsystems regression-green.
- Candidate 3352E385 built reproducibly and gated-deployed; safe B3705 preserved.


# Native-hands campaign -- phase status (2026-09-03)

## 2026-09-03: FLICKER ROOT-CAUSED (surface order), CONTENT-BASED IDENTITY, CALIBRATION, PHYSICAL AIM -- see docs/FLICKER-ROOT-CAUSE-2026-09-03.md
- Hardware run on 6A221ECD (log: logs/hw-2026-09-03/): hands and gun follow
  the controllers; ~45 % of hand solves refused `LANDMARK_RMS 0.0455 m`
  with `unknownBytes=0` on every attempt (VBO validity REFUTED as the cause).
  The raw refused sample shows vertex 909 holding what vertex 72 holds:
  the engine packs the two hand surfaces in EITHER order per frame. Offline
  reproduction on the BAR rig matches all four fingerprint numbers exactly.
  The visible flicker = coherent vanilla pairs (both hands refused on the
  same frame) accepted between native pairs; mixed pairs were refused.
- FIX: hand identity from CONTENT, never draw order. Every landmark table is
  tried in both half-orders inside the draw's own index range
  (`VRHandPose_SolveAnyEx` outSwapped/firstIndex, `VRHandPose_Identify`);
  the merged split takes its half classes from which order solved; single
  hand draws are whichever table solves; sleeves go to the palm their cuff
  is nearest (`GLSurf_TestSleeveNearest`). Declared order is only the
  fail-closed fallback. Merged SLEEVE batches now split whenever their hand
  is driven (before, only when hidden or probed -- sleeves were vanilla in
  every native pair).
- ONE VISUAL OWNER: per eye-scene the first hand draw of a class decides
  native/vanilla; a would-be second owner is kept coherent (vanilla copy
  after native: not drawn; native after vanilla: drawn vanilla and
  recorded so the verdict judges it). Counters `ownership:` in the log:
  primaryNativeDraw, primaryVanillaFallbackDraw, duplicateOwnerPrevented,
  pairNative, pairFailClosed, mixedEyeRejected, vanillaFallback
  aborted/accepted, order normal/swapped/unknown, singles/sleeves by
  content/proximity/ordinal.
- PAIR POLICY: an ENGAGED object both eyes drew and neither could own is
  `VANILLA_FALLBACK` -> refused (compositor holds the previous native pair)
  up to 3 consecutive pairs per object, then accepted as coherent vanilla;
  the engagement gate disengages a persistently refusing hand within ~2
  pairs anyway, so the degradation path is abort-or-two then plain vanilla,
  never a stale pose.
- CALIBRATION: `tools/derive_palm_calibration.py` -> `src/vr_palm_calibration.inc`:
  ControllerGripToPalm per hand per rig from the shipped skeleton (finger
  roots) and mesh (palm shell normal) against the OpenXR grip convention
  (k = +Y, t = +Z, n = handSign*X). Applied by default (`vr_native_hands_cal_table 1`);
  the Euler/offset config values are trims on top. The gun anchors to the
  CALIBRATED right palm + its own trim, so RIGHT calibration moves hand and
  weapon together with the authored palm->rear-grip relation preserved
  (residual 0.00000 u); LEFT calibration moves neither.
- PHYSICAL AIM (`vr_physical_aim 1`): the native gun's barrel (model
  barrelFwd under the drawn pose) is handed to vr_weapon as the weapon
  basis (`VRWeapon_SetBarrelOverride`); the existing decoupled aim model
  steers the game's own view angles (the state shots follow) through
  relative mouse while the camera is composed on the body basis + HMD.
  Offline: 20 deg right / 15 deg up / diagonal follow the rigidly rotated
  barrel to <0.1 deg, game aim converges <0.1 deg, camera base moves 0.000
  deg, head turn leaves the aim untouched, right-stick turn moves body,
  camera base and aim together, zero steering leaks.
- Gate 2026-09-03: **1752 safe + 131 DLL = 1883 passed, 0 failed**. Temporal
  soak 10k pairs with the surface order swapped on 10000 of 20000 game
  frames: 0 half-native submitted, 0 duplicate owners, 0 disagreements,
  0 NaN/stale/collisions, every RMS refusal an injected corruption.

## 2026-09-01: VBO CONTRACT + TEMPORAL STEREO CANDIDATE -- see docs/STEREO-TEMPORAL-2026-09-01.md
- VBO shadow: allocation generations + written-interval tracking; orphaned
  bytes are UNKNOWN; palm/gun readers refuse per landmark BEFORE the solver
  with the distinct reason VBO_BYTES_UNKNOWN (site 4). Contract cases A-F.
- The 0.0455 m fingerprint: deterministic sweep (tools/orphan_residual_sweep.py)
  finds NO constant pattern in the printed window; the orphan read gives
  0.04484 m. Fixed but historically UNPROVEN; the palmtry/objapply lines of
  the next normal Quest run decide it.
- Stereo: the 2026-08-29 gate was predictive (one pair late) and compared raw
  authored baselines (wrong). Replaced by ABSOLUTE per-eye targets with eye
  offsets, per-eye apply records, and a PRE-SUBMIT PairWorld verdict wired
  into SubmitBothEyesAndEnd: a half-native or disagreeing pair is ended with
  zero layers before xrEndFrame; the failing object is held vanilla one pair.
- Temporal suite in TRUE F1 order through the production submit path
  (tests/temporal_tests.inc): eye0-PASS/eye1-FAIL never reaches XR
  submission; 10k-pair temporal soak, 0 half-native SUBMITTED.
- Pair angle metric moved off acos(trace) (0.03 deg float floor).


## THE CONTROLLER-HANDS CANDIDATE IS BUILT AND GATED (2026-08-28)
Gate: **1386 safe + 131 dll, 0 failed**, including the new production
acceptance set and a 10,000-pair hostile soak through the ACTUAL production
functions (dropouts, weapon switches, headless pairs, grab/release churn:
zero NaN, zero pair-consistency violations, zero latch mismatches, zero
re-entries).

What the candidate does (all default ON, `codvr.cfg` documents it):
- RIGHT controller -> rendered RIGHT hand, full 6DoF (proven wiring).
- LEFT controller -> rendered LEFT hand, independent 6DoF.
- RIGHT controller -> the WEAPON, pinned at its measured STRONG rear grip,
  carrying the AUTHORED palm->grip relation re-measured every pair from the
  live baselines -- palm/rear-grip calibration residual asserted at
  exactly 0.00 mm. Unknown weapons / WEAK grips stay vanilla.
- LEFT squeeze near the fore-end -> sliding two-hand support (t in [0,1]
  along the measured support segment, R->L geometry sets pitch/yaw, roll
  stays anchored to the right controller, minimal-arc with an antiparallel
  guard, residual-detach, smooth release).
- Physical reload connected to the pair-latched LEFT controller through the
  measured per-weapon zones (body-bone frame): exactly ONE engine reload
  per completed gesture (VRBinds_TapControl delivery, counted), cancelled
  by menu/tracking loss/weapon switch/timeout; reacquisition cannot
  manufacture a commit.
- Fire direction: physical-shot redirection is DISABLED in this candidate
  (no mechanically proven camera-independent aim seam exists; the 37/38
  path stays retired). Stated per the lead's allowance.
- ALL THREE hand rigs covered (hands_new4 / hands_sten4 / hands4) via
  per-rig landmark tables + SolveAny; the hands4 rig's per-surface single
  draws take targets through the non-merged path; a mirrored palm
  structurally cannot solve.

## LATERALITY RE-CORRECTED (2026-08-28) -- READ BEFORE TOUCHING SIDES
The asset side IS the rendered side: surface 1 / `bip01 r hand` /
verts [837,1674) is the player's RIGHT (trigger) hand. Proof chain in
vr_glsurf.cpp MergedHalves(): the live hand modelview is the pure
Quake-view->GL permutation (det +1, submitted +x = eye-FORWARD); in that
space the solved surface-1 palm is the near/right/low trigger hand and
surface 0 sits 15 in FORWARD of it (the authored inter-hand distance; the
old mapping was physically unholdable); muzzle-distance physics agrees
across six weapons (tools/check_grip_hand.py); the profile rear grip was
measured from `bip01 r hand` -- the very hand the RIGHT controller drives.
The 2026-08-24 sweep's pixel-disjointness stands; its anatomical naming was
an eyeball error made in submitted-space coordinates.

## GUN TABLES RE-KEYED (2026-08-28)
The engine draws each gun SURFACE as its own batch with a LOCAL array
(measured: minIdx 0, spans sum to the model total), so the old model-total
keys could never match a live batch -- the gun solver was unwireable as
generated. kGunModels is now keyed by the BODY SURFACE's vertex count with
LOCAL landmark indices (24 models, 1 benign collision disambiguated by
residual), carries the two-hand support chain (supportM/barrelFwd/radius,
STRONG-gated) and the reload zones ride the same key, converted into the
body-bone frame through a rigidity-checked tag_weapon->body bridge.

## REAL PALM GATE: **PASS** (evidence: runs/vbogame-20260827-155348)
The 15:53 attended run executed the units-fixed build `6AAFBDF1` and reached
~35 s of instrumented gameplay before the harness (blinded by the logger's
fopen_s deny-all sharing -- see below) killed a HEALTHY run. Its log decides
the gate:
- `=== RIGHT PALM SOLVED ===`, `=== LEFT PALM SOLVED ===`,
  `=== REAL PALM PATH ACTIVE ===` all fired;
- **687 successful solves per hand** by run end, valid=1 poses at distinct,
  animation-tracking positions (e.g. hand_L pos(1.56 -4.08 -5.08) /
  hand_R pos(16.59 -1.12 -7.62), unit-norm axes, proper rotation by
  construction, rms under the scaled gate);
- VBO path again perfect: 206,634 vert + 5,431 idx resolves, 0 misses;
- refusals (all at the `solve` site per the new split counters) interleave
  with solves and match frames drawn with the SECOND hands rig (RIG_HANDS4,
  302-vertex halves) that has no landmark tables -- the designed fail-closed.
  Rig-2 landmark coverage is a production TODO, not a gate blocker; the
  next armed run's `palmdump` lines will carry the raw refused samples to
  confirm the attribution offline.

The flat-testing question ("does the corrected 39.3701 units-per-metre palm
solve succeed on the real stream?") is ANSWERED YES. No further flat run is
needed for this fact.

## 2026-08-27 evening: why the 15:53 watcher was blind -- fopen_s
The logger opened codvr_result.log with `fopen_s`, which denies ALL sharing:
no external reader of ANY share mode could open the log while the game
lived (proven live: 0/3 reads against the real DLL via selftest_host). That
one fact blinded both the 13:42 watcher (ReadAllText) and the 15:53 watcher
(shared-read FileStream), and my 13:42 "reader fix" mis-modelled the writer.
Fixes, all self-tested:
- Logger now `_fsopen(..., _SH_DENYWR)` -- live-readable, proven against
  the rebuilt DLL.
- **Module identity is now EXTERNAL and authoritative**: the harness
  enumerates the live pid's modules via `EnumProcessModulesEx(LIST_MODULES_
  ALL)` (the .NET `.Modules` property cannot see a 32-bit process from
  64-bit PowerShell -- caught by the self-test before it hit CoDSP) and
  requires the app-local `opengl32.dll` path whose on-disk file hashes to
  the deployed build, while the process is alive. No log line is needed.
- The internal `=== CODVR BUILD ID ===` block (which WORKED at 15:53 --
  lines 4-8 of that log -- and was NOT a loader-lock failure) is demoted to
  a cross-check; only a PRESENT-and-mismatched id may mean "wrong binary".
- New `=== PROXY FIRST CALL ===` milestone at the game's first
  wglCreateContext into the proxy.
- Two-phase harness: pre-click gate (PROCESS_STARTED ->
  APPLOCAL_OPENGL32_LOADED -> PROXY_FIRST_CALL -> PROBE_CONFIG_ARMED, 120 s
  budget, aborts BEFORE any user interaction) and an attended phase that
  NEVER aborts on a missing milestone.
- `VRHandPose_GetLastSample` + `palmdump` log lines: every future armed run
  keeps the raw observed landmark positions of the first refused and first
  successful attempt per hand, so solve questions become offline-decidable.
Suites: gate 1300 + 131 green (dist `959680B2...`), identity/milestone
17/17, lock handling 24/24.

## 2026-08-27: the attended run happened on the WRONG DLL -- no gameplay VBO data yet
The morning's attended flat session (archived: `runs/baseline-20260827`)
reached gameplay (46205 frames, scene capture ran) but was played on the SAFE
baseline `B3705FE5` -- post-run hygiene had already restored it after the
aborted 2026-08-26 vbotrace attempt, and nothing re-deployed the capture rig.
Its log has no `PROBE ARMED` banner and no vbo/glsurf lines: zero VBO-path or
palm evidence. Separately, the 2026-08-26 `vbotrace` run (a) never saw a scene
(`render=71 seen 0 frames` for all 25407 frames -- the mission briefing was
never clicked through) and (b) was armed WITHOUT `vr_glprobe_handsolve 1`, so
it could never have produced palm solves even with gameplay.

Fixes (both deterministic, both tested):
- `tools/flat_run.ps1` now RESTORES the safe checkpoint DLL and hash-verifies
  it at every collection (main path and `-Collect`), and `-Collect` also
  disarms codvr.cfg. The "scripted, verified" hygiene claim below is now true.
- `tools/vbogame_run.ps1` pins the full arming (glcap 2/90 + handsolve) so
  the next attended run cannot be launched half-armed. Use
  `-DeployOnly` / play / `-Collect`.

Gate re-verified 2026-08-27: 1294 + 129, 0 failed; dist rebuild reproduces
`53DBDC24` byte-for-byte. Real-palm hard gate: NOT passed (no data).
Native hands NOT deployed. Headset NOT launched.

## 2026-08-27 midday: ONE-TRANSACTION harness (`tools/vbogame_run.ps1`)
The two-command DeployOnly/Collect split is gone. One invocation now owns the
whole lifecycle: build+gate -> mechanical pre-flight (staged != safe baseline;
staged carries banner/handsolve/VBO strings; armed cfg carries every required
line; contract reporting present) -> save exact pre-run DLL+cfg with SHA256 ->
deploy -> re-hash the game folder and abort BEFORE launch on mismatch ->
launch -> wait for the in-game marker -> INSIST within 20 s on the new in-log
banner (`=== VBOGAME REAL PALM PROBE ACTIVE ===` with the deployed DLL's own
self-computed SHA-256, armed/handsolve/VBO-shadow YES) or kill the run ->
bounded linger -> collect -> ALWAYS (finally: success/failure/crash/timeout/
Ctrl-C) restore the exact saved pre-run files and hash-verify (exit 3 loudly
if that fails). A hard-killed transaction's leftover rig is recognised by
hash and self-recovered from the checkpoint on the next invocation.

DLL changes behind it (gate green 1294 + 131 after):
- `LogVbogameBanner()` in the first-scene block, armed configs only, with a
  CryptoAPI SHA-256 self-hash (`Sha256File`, pinned to the FIPS-180 'abc'
  vector by two new self-tests).
- The capture tick now honours `vr_glcap_quit_after` WITH a capture: after
  the 2-frame dump it lingers (palm/vbo stats every 600 scene frames, tagged
  `linger`, final `run-end` dump) until quit_after scene frames, so IDLE,
  FIRE and RELOAD all happen while the palm probe solves. Until now autoquit
  closed the game seconds into gameplay -- fire/reload evidence was
  structurally impossible.
- Armed recipe: glcap 2/90, quit_after 1890 (~30 s gameplay), handsolve 1.

## 2026-08-27 afternoon: noon launch failures diagnosed + lock gate added
Both noon attempts died at the deploy copy because **CoDSP.exe was running at
that exact moment** -- Steam's console_log shows app 2620 running 11:58:44-
11:59:33 and 12:29:39-12:30:04 (PID 29716), bracketing the harness's prerun
saves at 11:58:50 and 12:29:40. The game had been started BY HAND alongside
the harness (the old DeployOnly workflow taught "start the game whenever you
like"); its loaded opengl32.dll allows reads (prerun save worked) but denies
writes (deploy failed). Killing CoDSP/CoDMP DID release the lock -- each
retry just re-acquired it with a fresh manual launch, and the old harness
named no owner, retried nothing, and swept leftover processes AFTER the
first mutation.

Fixes, all in `tools/vbogame_run.ps1` and self-tested:
- **Lock gate before any mutation**: Restart Manager (`RmGetList`) + a
  process-module scan name every owner (pid/name/path/classification);
  safe-listed CoD transients (CoDSP/CoDMP) are closed BY PID, gameoverlayui
  is waited out, UNKNOWN owners abort pre-mutation with the exact owner
  printed. A write-access probe must succeed before anything is touched.
- **Staged, verified replacement** for deploy AND restore ([IO.File]::Replace
  with hash checks on stage and destination), restore behind the same lock
  gate with a bounded retry budget; restoration success is claimed only
  after both hashes verify (exit 3 + loud report + preserved prerun copies
  otherwise).
- The harness prints up front: do NOT start the game yourself -- it launches
  the game itself only after deployment is hash-verified.
- `tools/test_vbogame_locks.ps1`: 24-check suite driving the REAL harness
  path (-GameDirTest sandbox) through cases A-H (clean run; unknown owner
  named + pre-mutation abort + byte-identical DLL; safe owner auto-cleared;
  restore through a temporary lock; unrecoverable restore -> exit 3 with
  backup kept; exit 0 <=> restored). 24/24 green 2026-08-27.

## 2026-08-27 13:42 attended run: the probe RAN PERFECTLY; the watcher was blind; palms refused on UNITS
Evidence preserved in `runs/vbogame-20260827-134249` (result log, 272-record
draw CSV, console.log, Steam launch log, prerun copies). Timeline: 13:43:12
launch -> :14 CoDSP pid 27848 -> :30 first 3D scene + marker + BANNER ->
:31 capture CSV dumped -> :52 harness-killed clean exit.

What the log PROVES about that run (all with the 8295EC02 build id in-log):
- The instrumented DLL was loaded and armed: the full VBOGAME banner IS in
  the log with the exact hash, `handsolve: YES`, `VBO shadow: YES`.
- The REAL gameplay VBO path works end to end: vboOk 136458 vert + 3631 idx
  resolves, 0 misses, nullPage 0, 1 game context, BufferData(NULL) orphan +
  SubData(arr) update path, no Map/Unmap; viewmodel classified every frame
  (gun 5985, hand batches 1197 + 1197) and every merged-batch split verified.
- The palm probe ran on every hand draw and REFUSED all 1197 per hand.

Root causes, both fixed and pinned by tests:
1. WATCHER (why the run was killed): `[IO.File]::ReadAllText` opens with
   FileShare.Read, which CONFLICTS with the game CRT's open write handle --
   every poll threw a sharing violation into a swallowed catch, so the
   watcher read nothing while the banner sat in the file, and a healthy run
   was aborted as "wrong binary". Fixed: `tools/vbogame_lib.ps1` reads with
   FileShare ReadWrite|Delete from byte 0 every poll.
2. PALM SOLVE (the real defect): landmark tables are in METRES (extractor
   divides by 39.3701) but the engine submits GAME UNITS (1 unit = 1 inch,
   proven); rotation-only Procrustes has no scale term, so the residual gate
   (4 mm) could only refuse -- 0 solves / 1197 refusals, while the offline
   suite stayed green because its synthetic batches were built FROM the
   metre landmarks (mock-reality units mismatch). Fixed: every solve takes
   an explicit `unitsPerMetre` (`CODVR_UNITS_PER_M` = 39.3701 on the live
   path); gate scales with it; the exact live failure is a pinned MUTANT
   test ("engine-unit batch REFUSED by a metre-assuming solve").

New milestone protocol (DLL logs each once; harness names the deepest stage
instead of guessing): `=== CODVR BUILD ID ===` (self-SHA at ATTACH -- the
only line that may ever mean "wrong binary"), `=== VBOGAME PROBE CONFIG ===`
after parse, `=== FIRST 3D SCENE ===`, `=== FIRST VIEWMODEL DRAW ===`,
`=== VBO SHADOW RESOLVED ===`, `=== RIGHT/LEFT PALM SOLVED ===`,
`=== REAL PALM PATH ACTIVE ===`. Hand-solve refusals are now split by site
(idx/base/solve) in the palm stats line. Self-tests:
`tools/test_vbogame_milestones.ps1` (17 checks, incl. the writer-held-log
regression) + the 24-check lock suite.

The ONE fact no offline artefact can supply (the CSV stores draw metadata,
not vertex streams): whether the real-stream palm solves now PASS for both
hands. Everything upstream of that is already proven by the 13:42 run. One
more attended flat run answers it -- pending project-lead authorization; the
command is unchanged: `tools\vbogame_run.ps1`, click Start, idle ~3 s, fire
once, reload once, leave it alone. Fire/reload are NOT automated deliberately: the mod has
no engine command-buffer path, and inventing one via new RE of the packed exe
right before the one authorized run risks crashing it.

Canonical tree: `H:\Projects\COD-VR-render-hands`
Deployed (game folder): `B3705FE5...` -- the safe pre-glsurf baseline, hash-verified after every flat run.
Gate: **1294 safe + 129 dll, 0 failed.**

Target architecture (RTCWQuest-validated): right controller = weapon pose,
left grip = two-hand stabilisation, weapon forward = bullet direction, HMD =
camera only.

## Phase 1 -- real CoD VBO access: HALF DONE
- getproc census (menu run, `runs/vbotrace`): the engine requests the FULL
  ARB buffer family -- Gen/Delete/Bind/IsBuffer/BufferData/BufferSubData/
  GetBufferSubData/Map/Unmap/GetBufferParameteriv/GetBufferPointerv. All the
  mutating ones are wrapped.
- Observed update path so far (menu): **`BufferData(NULL)` orphan +
  `BufferSubData` ranges, GL_STREAM_DRAW**, small buffers, heavy churn.
- **Discovery that changed the design: more than one GL context renders
  through the proxy** (game + Steam overlay). Buffer ids are per context, so
  the mirror is now keyed by (context, id) via wglMakeCurrent; the first
  global-table build pinned 512 live buffers and would have served one
  context's bytes for the other's ids.
- STILL NEEDED: one attended flat gameplay run (a single click through the
  mission briefing) to trace the path the VIEWMODEL uses and confirm shadow
  hit rates on real gun/hand draws.

## Phase 2 -- shadow VBO data: DONE (offline-proven)
`src/vr_vbo.{h,cpp}`: CPU mirror of every engine upload (Data / SubData /
Map->Unmap sync / Delete / context-delete), fail-closed staleness, byte and
table caps, reason-split counters. The glsurf readers (`SampledVertex`,
`BuildHandDelta`, `TransformDrawScratch`, `IndexRange`) resolve through it;
no VBO offset is ever CPU-dereferenced (S_VboShadow, 10 cases; soak
extended). Index arrays are now probed/resolved too (the crash-class gap the
audit found). Scratch route refuses VBO-backed arrays by design.

## Phase 3 -- prove real palm access: VEHICLE READY, RUN PENDING
`vr_glprobe_handsolve 1` (armed configs only) keeps zero-delta hand targets
installed so the palm solve runs on the real vertex stream; solves/refusals/
poses land in the stats dump (`glsurf [x]: palm ...` lines). Run it in the
same attended flat run as Phase 1's gameplay trace; ask the player to idle,
fire, and reload once each while it runs.

## Phases 4-6 -- pair-latch + right/left hands: WIRING REHABILITATED
The 2026-08-25 wiring is OUT OF QUARANTINE (see quarantine/README.md):
- Its four FAILs are attributed: one wrong test contract, three from a real
  **engagement deadlock** (solve needed engagement, engagement needed a
  solve). Fixed with `GLSurf_SetHandSolveWanted` -- first enabled pair seeds
  the baseline, second engages. Two-pair convergence is the tested contract.
- End-to-end tests pass: right-hand 6DoF landing (position AND rotation),
  left independence, fail-closed on no-tracking/no-head/no-baseline,
  one-sample-per-pair, stereo invariance incl. the reuse-mutant.
- 10k hostile soak through the full wiring: 0 faults, 0 re-entries.
- Enabled by `vr_native_hands 1` in codvr.cfg (per-pair call already in
  vr.cpp). NOT yet defaulted on; that is a candidate-build decision.

## Phases 7-13 -- NOT STARTED THIS PASS
Gun-follows-controller (rear-grip solve), two-hand support, control map,
fire direction, runtime diagnostic block (partially exists: wall-beat vbo
lines + palm lines), graphics untouched as directed.

## Standing hygiene
- Every flat run ends by restoring `checkpoints/deployed-as-found-2026-08-24`
  into the game folder and verifying `B3705FE5...` -- scripted, verified.
- dist/ holds the gated dev build; it is NOT deployed.
