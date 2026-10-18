# CODVR hardware run 2026-08-23 — stereo corruption autopsy

Source log: `H:\SteamLibrary\steamapps\common\Call of Duty\codvr_result.log` (2454 lines)
Build under test: `opengl32.dll` sha256 `88999c18…5185` (byte-identical to `build/` and `dist/`,
so `src/` at this commit IS the tested build).

## Verified log facts

| Fact | Line | Value |
|---|---|---|
| runtime IPD | 372 | 0.0624 m -> expected 2.456 u |
| rendered eye separation | 372 | **30.730 u** (12.5x) |
| eye0 XR pos | 377 | -0.0050 +0.0064 +0.0070 m |
| eye1 XR pos | 378 | +0.0572 +0.0084 +0.0109 m |
| **XR eye delta** | — | **0.0622 m — CORRECT** |
| eye0 rendered origin | 364 | -395.6 7258.2 -423.4 u |
| eye1 rendered origin | 366 | -421.4 7261.3 -439.9 u |
| game eye origin -> eye1 | 292 | -421.3 7260.2 -439.9 -> offset **-0.1 1.1 0.0** |
| body-yaw rebases | — | **1086**, from only **2** engage events |
| entity rewrites | 1694 | **7696**, `nearest 0.0 u`, 100048 deduped |
| steering cpd | 1696 | -9.09 -> **-0.7** (13x over-gain), effectiveness 194% |
| eye pair frames | 340/349 | eye0 = game frame **3032**, eye1 = game frame **3033** |

**The OpenXR eye poses were never wrong.** They are 0.0622 m apart, exactly the IPD.
The *rendered* origins are 30.730 u apart. The corruption is entirely game-side.

## Root cause A — no per-XR-pair latch of the base camera (THE FATAL ONE)

`src/dllmain.cpp:754` reads the base camera **live, per game frame**:

```c
float gameOrigin[3] = { fv[RD_ORG], fv[RD_ORG+1], fv[RD_ORG+2] };
...
const float* camBase = gameAxis;
if (VRWeapon_GetViewBasisRows(bodyBase)) camBase = bodyBase;   // dllmain.cpp:800
VR_GetEyeCamera(eye, gameOrigin, camBase, ...);
```

and `src/vr.cpp:2265` composes:

```c
outOrigin[i] = gameOrigin[i] + rotated[i];   // rotated = +-IPD/2 + clamped head lean
```

The F1 scheduler spans one XR frame across **two game frames** — eye 0 on frame N,
eye 1 on frame N+1. Because the base pose is resampled on each of those frames,
**every change to the game camera between them is rendered as stereo disparity**:

* a position change becomes a fake IPD;
* an orientation change becomes **independent per-eye rotation**.

`VRWeapon_Update` and `VRBinds_Update` also run once per *game* frame
(`dllmain.cpp:1620`), i.e. **twice per stereo pair**, so the body basis is
guaranteed to advance a full step between the two eyes.

This alone explains the user's "each eye rotated continuously in a loop, not in sync".

## Root cause B — syscalls 37/38 write the camera

Per the completed contradiction audit: **37 = CM_PointContents, 38 =
CM_TransformedPointContents**, and the pointer argument is `&cg.refdef.vieworg`.

`EntityForward` (`dllmain.cpp:680`) treats that pointer as an entity struct:

```c
float* org  = (float*)p;                        //  == cg.refdef.vieworg
float* axis = (float*)((BYTE*)p + 12);          //  == cg.refdef.viewaxis  (ENT_AXIS_OFF=12)
if (IsOrthonormal3x3(axis)) { … VRWeapon_MaybeRewriteEntity(org, axis, …); }
```

Two independent gates that were supposed to reject non-entities **cannot fail** here:

1. `IsOrthonormal3x3(p+12)` reads the real `viewaxis` — orthonormal by construction,
   so the shape gate always passes. Log line 275: `37: 2 entity-shaped / 38: 12 entity-shaped`
   — **100 %** of collision queries classified as entities.
2. `ClassifyViewWeaponEntity` computes `local = entOrigin - g_gameVieworg`, but
   `entOrigin` **is** the camera, so `local == 0` -> distance 0 -> class 1 "near" ->
   always the view weapon. Log line 1694 records exactly this: **`nearest 0.0 u`**.

Result: **7696 write-through rewrites of `cg.refdef.vieworg` / `viewaxis`** with the
weapon pose. The proximity classifier did not merely mis-tune — it was structurally
incapable of rejecting the camera.

## Root cause C — runaway body-yaw rebase (1086 from 2 engagements)

`src/vr_binds.cpp:808`:

```c
if (fabsf(obsYaw) > 15.0f && fabsf(expYaw) < 5.0f)
    VRWeapon_RebaseToGameAim();          // "spawn or scripted turn"
```

`expYaw = g_prevInjX * g_dpcYaw`, and `g_prevInjX` is **zeroed on every call**
(`vr_binds.cpp:852`) *before* the `if (!gameplay || !haveAim) return;` early-out.
Any call that does not reach the injection site leaves `expYaw == 0` while `obsYaw`
still carries the real, injected turn — so the "far beyond anything we injected"
test fires on motion the loop itself caused. Observed step: a steady **~25.4 deg
per rebase** ramp (log 382-389), later wrapping the full +-180 range.

## Root cause D — poisoned steering calibration (the amplifier)

The `else` branch of the same test adapts `g_dpcYaw` from `obsYaw / g_prevInjX`.
Fed by the rebase ramp, counts-per-degree drifted **-9.09 -> -0.7**: each degree of
aim error injected **13x more mouse counts**. Log line 1696 reports
`effectiveness 194%` — observed turn ~2x intended.

## The closed loop

```
game aim yaw
  └─(C) rebase ──► g_bodyYawDeg
                     ├──► VRWeapon_GetViewBasisRows ──► camBase ──► CAMERA BASIS
                     └──► weapon world rows ──► target yaw
                                                  └──► steering error
                                                        └──(D over-gained)──► mouse injection
                                                              └──► game aim yaw ──┐
   ◄──────────────────────────────────────────────────────────────────────────────┘
```

*(A)* samples this loop at **two different phases** for the two eyes -> the eyes diverge.
*(B)* additionally stomps `cg.refdef.vieworg` mid-frame, and that stomped value is what
the *next* frame's `fv[RD_ORG]` read picks up -> position noise enters the same loop.

Log line 359 is the loop caught in the act:
`STEERING LEAK -- injected aim moved the rendered camera 0.89 deg beyond body+head`.

## Why 30.730 u, and the laterality verdict

eye1 = gameOrigin(frame 3033) + 1.1 u  <- the +-IPD/2 offset math is **correct and small**.
eye0 = gameOrigin(frame 3032) + its own offset.
The two `gameOrigin` samples differ by ~30.7 u because (B) stomped `vieworg` with the
weapon origin between the frames and (C) swung the body basis ~25.4 deg between them
(the grip offset `w[i]*scale` rotates by `bodyYaw`, so a 25 deg rebase sweeps the
stomped origin through a long arc). The genuine +-1.23 u IPD term is buried under a
~30 u error term.

**Correction to the brief:** the log's `WRONG-SIDE` label is imprecise on this sample.
`vr.cpp:2898` scores `latOk = eye0LeftBy > 0 && |eye0LeftBy - wantU| < 0.25`, and the
measured value was **+17.041 u — correct sign, wrong magnitude**. The eyes were not
swapped in this sample; they were 7x too far apart along view-left. The single label
conflates side-reversal with magnitude error and is split in the fix. (Once the base
pose differs by 30 u, sign reversal *is* reachable on other samples — it just is not
what this line proves.) Note also that `left[]` is averaged across the two eyes' bases;
with bases 25 deg apart that average is itself meaningless — another symptom of (A).

## Fresh-eye and submission collapse

| interval | game fps | XR submitted | fresh L/R | verdict |
|---|---|---|---|---|
| healthy | 219.6 | 109.8 | 109.8 / 109.8 | MARGINAL |
| degraded | 125.5 | 117.7 | **7.8 / 7.8** | **FAIL** |
| final | 110.0 | **0.0** (begun 110/s) | 0 / 0 | skip 1200 |

* **Fresh-eye collapse is a consequence, not a scheduler defect.** The F1 pair machine
  delivered 109.8/109.8 fresh when the game supplied 219.6 fps. It needs *two* game
  frames inside one 8.33 ms XR frame; at 125.5 fps the game supplies barely one, so
  pairs stop completing. The proximate trigger is the game's own frame rate halving
  (219.6 -> 125.5) while the camera was being stomped into geometry and the injection
  storm was running. **The exact mechanism of the fps drop is not proven by this log** —
  instrumentation is added rather than guessed at.
* **`XR submitted 0 / skip 1200` is NOT a bug.** `vr.cpp:1848` increments that counter
  only on `!g_frameState.shouldRender` — the runtime telling us not to draw
  (headset off / dashboard up). The frame loop stayed alive and correctly submitted no
  layers. This is the user removing the headset. It must not be "fixed".

## Verdict

Two independent architectural defects, either of which alone produces nausea:

* **A** makes the stereo pair a *sampling* of a moving camera instead of one pose plus
  two eye offsets. Any camera motion becomes eye divergence.
* **B** gives an experimental weapon feature a write handle on `cg.refdef.vieworg`.

C and D supplied the motion that A converted into eye divergence, and B fed it back in.

---

# The fix: CODVR CAMERA-STABLE BASELINE

## Camera source of truth (`src/vr.cpp`)

ONE base camera pose is latched per XR pair. Eye 0 latches; eye 1 reuses.
`VR_GetEyeCamera` shadows its own arguments so nothing below the latch can
reach the live game camera:

```c
if (eye == 0 || !g_baseLatchValid) { latch(gameOrigin_, gameAxis_); }
const float* const gameOrigin = g_baseLatchOrigin;
const float* const gameAxis   = g_baseLatchAxis;
```

The invariant this restores:

    eyeOrigin[e] = latchedBase + R(latchedBasis) * (headLean +- IPD/2)

Both eyes share the base BY CONSTRUCTION, so the only term that can differ
between them is the runtime's own eye offset. A controller event, a steering
injection, a body-yaw rebase or a collision-hook write occurring between the
two game frames is now structurally incapable of changing eye separation --
it lands on the NEXT pair's latch and moves both eyes together, which is
ordinary camera motion rather than divergence. `ResetPairState` clears the
latch so a broken pair re-latches at eye 0.

## Retired 37/38 path (`src/dllmain.cpp`)

`RETIRED_INVALID_37_38_WEAPON_PATH 0` -- the forwarder is compiled out and
`EntityForward` is absent from `dllmain.obj`. `SetupWeaponDivertSet` flags
nothing; the asm divert branch is a transparent `jmp g_realSyscall`. The
invariant counter `collisionHookCameraMutationCount` is permanent, must read
zero, and the stereo gate treats nonzero as fatal.

## Stereo fail-safe (`src/vr.cpp`, before every submit)

Refuses a pair on: collision-hook camera mutation, non-finite basis, missing
pair latch, separation > 1.5x or < 0.5x the runtime IPD, reversed laterality,
or eye bases more than 2 deg apart. A refused pair is not submitted (the
runtime reprojects the last good one), and the first refusal disables every
experimental controller-weapon feature for the session.

Two measurement corrections were needed for the gate to be sound:

* **Expected separation comes from the poses the composition ACCEPTED**, not
  the raw located ones. The per-eye continuity gate substitutes a last-sane
  sample for an absurd one; scoring against raw poses condemned the very
  correction that saved the image (three suite scenarios proved this).
* **Eye divergence uses the chord form** `2*asin(|f0-f1|/2)`, not
  `acos(dot)`. Near-parallel axes make `acos` ill-conditioned: one float ULP
  in the dot product of two IDENTICAL bases reads as 0.02 deg of divergence.
* **Laterality is scored on eye 0's own left axis**, not an average of both.
  With divergent bases the average is meaningless -- which is exactly why the
  shipped check could not distinguish a side reversal from a magnitude error.

## Steering disarmed (`src/vr_binds.cpp`)

`vr_experimental_weapon` defaults 0. While off, `UpdateSteering` returns
immediately: no injection, and no calibration or rebase evaluation on motion
it did not cause. Locomotion and buttons are untouched. The lockout latched by
the fail-safe survives a config re-enable.

`EnforceCameraStableBaseline()` runs after config parsing **on every exit
path**, including a missing or unreadable `codvr.cfg`, and the module-level
defaults (`g_enabled`, `g_transformEnabled`) are now 0. The safe state is what
you get with no configuration at all.

## Rebase governor (`src/vr_binds.cpp`)

Every rebase carries a reason, is counted, and is rate-limited to 3 per 2 s;
refusals are counted separately and a storm is logged once. The expectation
the heuristic tests against is now an accumulator cleared ONLY when a fresh
aim sample is credited against it, and counts-per-degree is clamped to
[-0.5, -0.005] so the calibrator cannot run away again.

**Honest limitation.** The 1086-rebase runaway is NOT reproduced in the test
suite. The heuristic is unsound by inspection and is fixed, but the frame
timing that made it fire at 120 Hz depends on the game applying injected mouse
over several frames, and the mock harness applies it instantly and samples the
aim every non-menu frame, so the per-call record and the accumulator never
diverge there. The log does not characterise that latency well enough to model
honestly. What IS pinned, and what makes a runaway impossible regardless of
what the heuristic decides, is the governor.

## Verification

* Gate green: **856 (safe suite) + 129 (DLL suite) = 985 checks**, 0 failed.
* `-GateSelfCheck` confirms the gate itself still fires on a red log.
* **Mutation-tested.** Restoring the shipped per-eye resample turns 8
  assertions red and measures **25.4002 deg** of basis divergence -- the exact
  hardware step -- while the stereo fail-safe independently rejects the pairs
  and locks out the feature. Two layers catch it.
