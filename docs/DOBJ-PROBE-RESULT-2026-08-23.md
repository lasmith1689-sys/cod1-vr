# DObj bone-controller probe — MEASURED RESULT: **REFUTED**

**Date:** 2026-08-23
**Run:** automated flat-monitor, headset off, `spmap chateau` (Thompson — the rig the
probe was designed against, 10.75x weakest-pair margin)
**Build:** `dobj-flat-probe` checkpoint, `opengl32.dll` sha256 `68d63072…`
**Answer to U8 ("does the first-person render path honour controllers at all?"): NO.**

---

## Verbatim result

```
dobjprobe: view DObj 0x0088AE88 (weapon 4), probing tag_brass [57], parent tag_weapon [4]
dobjprobe: quiescent. drift 0.00000 u / 0.00000 deg per frame; tolerance 0.10000 u / 0.50000 deg

dobjprobe: strategy 1/4: 160 with a 168/170/149 pose scope (the engine's own path)
dobjprobe:   syscall 160 (enable controller) ctx 0206F928 bone 57  -> 0
dobjprobe:   after one evaluation the bone moved 0.00000 u / 0.00000 deg
dobjprobe: strategy 2/4: 159 with a 168/170/149 pose scope
dobjprobe:   syscall 159 (enable controller) ctx 0206F928 bone 57  -> 0
dobjprobe:   after one evaluation the bone moved 0.00000 u / 0.00000 deg
dobjprobe: strategy 3/4: 160 with a zeroed context, no scope
dobjprobe:   syscall 160 (enable controller) ctx 0206F928 bone 57  -> 0
dobjprobe:   after one evaluation the bone moved 0.00000 u / 0.00000 deg
dobjprobe: strategy 4/4: 159 with a zeroed context, no scope
dobjprobe:   syscall 159 (enable controller) ctx 0206F928 bone 57  -> 0
dobjprobe:   after one evaluation the bone moved 0.00000 u / 0.00000 deg

dobjprobe: no enable path produced a change.
           The first-person DObj does not honour bone controllers.

======= DOBJ CONTROLLER CONVENTION PROBE =======
DObj         : 0x0088AE88   weapon index 4
structure    : tag_view 0  tag_torso 1  tag_weapon 4
probe bone   : tag_brass (index 57)   parent ref tag_weapon (index 4)
drift        : 0.00000 u / 0.00000 deg per frame over 8 still frames
tolerance    : 0.10000 u / 0.50000 deg
  A  org ( 9.1451 -3.1597 -3.0448)  quat (-0.111732 -0.105401 -0.716770  0.680182)
  R  org ( 2.7291 -2.4895 -3.4738)  quat (-0.000043  0.000000 -0.000915  1.000000)
probe controller:
  quat (-0.341236 0.740771 0.191739 0.545943) = 113.822 deg
  trans ( 6.0000 -4.2500 2.5000) = 7.7661 u
  B  org ( 9.1451 -3.1597 -3.0448)  quat (-0.111732 -0.105401 -0.716770  0.680182)
  change from A: 0.00000 u  0.00000 deg
```

## What this establishes

| Fact | Status |
|---|---|
| The view DObj is reachable from cgame (162) | **PROVEN — works** |
| Bone name -> index on the view DObj (174) | **PROVEN — works** (`tag_brass` -> 57) |
| Evaluated bone transforms readable (157) | **PROVEN — works**, real data, stable to 0.00000 u over 8 frames |
| Controller *enable* on the view DObj (159/160) | **PROVEN — REFUSED.** 4/4 paths return 0 |
| Controller composition convention (U1–U4) | **UNMEASURABLE — and moot.** Nothing composes |
| DObj route as the first-person weapon carrier | **REFUTED** |

The refusal is not a context/scope problem: strategy 1 used the engine's own
168 -> 170 -> 149 -> enable -> 171 sequence, the exact path the turret writer at
cgame `0x13F70` uses successfully on world entities, and it was refused too.
The most probable mechanism is that the view DObj is created (`DObj_Create`)
with **zero controller slots**, so the enable can never succeed — that is an
allocation decision inside the DRM-packed `CoDSP.exe`, not something cgame can
change.

## Consequence for the native-hands milestone

`docs/DOBJ-CARRIER-DECISION.md` and the offline package both stated the rule:

> **Until PASS, no production DObj weapon controller exists and none may be written.**

The probe did not return PASS. It returned the refutation. So:

* No production DObj weapon controller is written.
* No DObj-driven visible hands are written.
* The two-hand rigid solve, physical sights and physical reload all consumed the
  DObj carrier as their output stage; with the carrier refuted they have nothing
  to drive, so they are **not** shipped as dead code pretending to work.

Writing them anyway would produce a build whose "hands" provably cannot move a
single vertex — the same class of error as the retired 37/38 weapon path, which
also passed its own tests while doing nothing the user could see.

## The other outcome of this run — automation is now UNBLOCKED

The prior finding "the game cannot be launched programmatically" is **superseded**.
With the Steam client already running:

```
steam.exe -applaunch 2620 +set com_maxfps 60 +spmap chateau
```

launches CoD1 SP, forwards the arguments, loads a gameplay level with a weapon in
hand, and the mod hooks cgame normally. Verified this session.

* Direct `CoDSP.exe` exec still fails (DRM modal, proxy never loads) — must go through Steam.
* `spmap` from `Main/autoexec.cfg` does **not** work (even behind `wait 300`); it must
  arrive as a `+spmap` startup argument.
* `chateau` works and has a save; `brecourt` did not load.

Flat, self-scoring, headset-free experiments are therefore now **cheap and repeatable**.
That is the tool needed to find whatever mechanism *does* drive the first-person
weapon, without spending a play session on it.
