# Render ownership and the ~50% palm refusal — 2026-08-29

Offline cycle. The game was never launched, the headset was never launched, no
attended run happened. The game folder carries the SAFE baseline
(`B3705FE5…` / `F7AB7A2D…`) throughout.

---

## 1. The ~50% palm refusal — CASE C, root cause narrowed, one defect fixed

### What the evidence actually says

From the 2026-08-28 run (`codvr_result.log`), every heartbeat:

```
R{... solve 3233/3085 rig -1}  L{... solve 3233/3085 rig -1}
palm refusals R idx/base/solve 0/0/3085
lastRef{site 3 idx=7824 span=1674 bestRms 0.0455m} (idx=7824 span=1674 x3085)
```

Five facts follow, and they are load-bearing:

1. **`idx=7824 span=1674` is the merged hand batch and nothing else.**
   `7824 = 2 × GLS_HAND_INDICES`, `1674 = 2 × GLS_HAND_VERTS`. It is the only
   refusing shape in the histogram. This is the **primary visible hand draw** —
   there is no second pass, no alternate rig, no LOD to blame. **CASE C.**
2. **Exactly 2 attempts per class per pair** (1200 attempts per 600 pairs) —
   one merged draw per eye-scene. No duplicate pass exists to be excluded.
3. **R and L are identical on every heartbeat**, and they are printed from
   separate variables (`hr` / `hl`). Both classes solve, or neither does — a
   whole-array property, not a per-hand one.
4. **The rate varies with gameplay** (56%, 62%, 41%, 49%, 46% across
   consecutive 600-pair windows). Not a clean eye alternation.
5. **`refusedIdx = 0`, `refusedBase = 0`, all at site 3 (the solver)**, and
   `vboOk 834428 / vboMiss 0`. The indices read, the span resolved, the shadow
   *vouched* — and the solve still refused.

### What was eliminated, by measurement

`tools/crosssolve_palm.py` and `tools/crosssolve_sweep.py` run the production
Kabsch against the real shipped geometry over 14–22 animation states:

| hypothesis | residual | verdict |
|---|---|---|
| correct table on correct geometry | **0.00000 m** | solves |
| hands_sten table on hands_new geometry | **0.00000 m** | the two rigs are byte-identical — rig ambiguity cannot cause a refusal |
| cross-hand (trigger table on support geometry) | 0.05253 m, constant | not 0.0455 |
| mirrored geometry (chirality), any axis | 0.0303 m, constant | not 0.0455 |
| wrong-half index shift (same local idx, other surface) | 0.0426–0.0718, **varies** | not constant |
| half-stale cloud (two anim frames) | 0.000–0.227, **varies** | not constant |
| base-offset sweep, k ∈ [−1000, +1000] | none constant AND on target | no offset explains it |

So: **not the tables, not the rig, not laterality, not chirality, not units,
not a wrong-half index, not a split-frame read.**

### What survives

The refusal is **constant** across thousands of draws and across varied
gameplay. A constant Procrustes residual means the observed cloud is *rigid*
with respect to the bind cloud — it is a fixed wrong point set, not noise and
not a partially-updated buffer. The only remaining input that can differ
between two draws of the same batch with the same tables is **the vertex bytes
themselves**.

### The defect found in exactly that path — FIXED

`VBO_NoteBufferData` handled the orphaning idiom (`glBufferData(size, NULL)`)
by zero-filling the shadow **and clearing `stale`**:

```c
if (b->stale) { --g_vstats.staleBuffers; b->stale = 0; }
...
if (data) memcpy(b->shadow, data, b->size);
else      memset(b->shadow, 0, b->size);      // and vouched for it
```

GL says the store's content is **undefined** after an orphan. Vouching for
those zeros hands the palm solve 48 identical points. That is not a crash and
not a miss — it is a *silent wrong answer*:

* the covariance matrix is zero, Horn's quaternion degenerates to identity,
* the residual becomes the **bind cloud's own radius of gyration**,
* and `vboMiss` reads **0** the whole time.

Measured bind gyrations of the shipped tables:

| table | gyration |
|---|---|
| `kPalmTRIG` / `kPalmSTEN4_TRIG` | **0.044837 m** |
| `kPalmSUPP` / `kPalmSTEN4_SUPP` | 0.044075 m |
| `kPalmH4_TRIG` | 0.050679 m |

The runtime reported **0.0455 m**. The trigger table's gyration is **0.0448 m**
— within 1.5%, the right order, constant by construction, and produced by a
real defect on the exact code path the evidence points at.

**This is not yet proof.** 0.0448 ≠ 0.0455 at the precision the log prints, so
the degenerate-cloud mechanism explains the *shape* of the failure but not the
last 1.5% of its magnitude. Confirming or refuting it needs one run, and this
cycle is forbidden from launching the game.

**The fix, regardless:** the shadow now tracks the written run `[fillLo,
fillHi)` since the last orphan and **refuses** to resolve bytes nothing has
written. A disjoint write conservatively restarts the run — that can only cause
extra refusals, never a false vouch. New counters `resolveMissUndefined`,
`orphans`, `fillRestarts` are logged per wall beat.

### The diagnostic gap is already closed

The log quoted above predates the current source. `vr.cpp` now prints
`shape` (mirror-invariant radius of gyration of the *observed* cloud) and the
`vbo=`/`off=` provenance for both the refusing and the succeeding draw. The
next run discriminates in one line:

* `lastRef{shape 0.0000}` → **the degenerate/undefined read is confirmed** (and
  the fix above removes it);
* `lastRef{shape ≈ 0.0448} == lastOk{shape}` → same geometry, wrong
  correspondence;
* differing shapes → genuinely different geometry, and `vbo=`/`off=` says
  where it came from.

---

## 2. Render ownership, derived from the shipped assets

`tools/derive_render_ownership.py` walks
`surface → vertex → skin weights → bone ANCESTRY → triangle ownership`.
Bone **names are never consulted**: this rig contains `r cuff01` and
`r wrist01`, both children of `bip01 l hand`. A name-based partition puts two
cuff bones on the wrong arm.

### `viewmodel_hands_new4` / `viewmodel_hands_sten4` (1914 verts)

| surface | verts | range | tris | ancestry | pure | cross-arm verts | seam tris | class |
|---|---|---|---|---|---|---|---|---|
| 0 | 837 | [0, 837) | 1304 | L (`bip01 l hand`) | yes | 0 | 0 | `VIEW_LEFT_HAND_ARM` |
| 1 | 837 | [837, 1674) | 1304 | R (`bip01 r hand`) | yes | 0 | 0 | `VIEW_RIGHT_HAND_ARM` |
| 2 | 120 | [1674, 1794) | 158 | R | yes | 0 | 0 | `VIEW_RIGHT_SLEEVE` |
| 3 | 120 | [1794, 1914) | 158 | L | yes | 0 | 0 | `VIEW_LEFT_SLEEVE` |

### `viewmodel_hands4` (790 verts) — declares its surfaces in a *different* order

| surface | verts | range | tris | ancestry | class |
|---|---|---|---|---|---|
| 0 | 93 | [0, 93) | 133 | R | `VIEW_RIGHT_SLEEVE` |
| 1 | 93 | [93, 186) | 133 | L | `VIEW_LEFT_SLEEVE` |
| 2 | 302 | [186, 488) | 383 | R | `VIEW_RIGHT_HAND_ARM` |
| 3 | 302 | [488, 790) | 383 | L | `VIEW_LEFT_HAND_ARM` |

**Every surface of every rig is structurally pure**: 100% of its vertices reach
exactly one arm root, zero cross-arm-weighted vertices, **zero seam triangles**.
The documented ambiguous-triangle tie policy (majority of three) is therefore
defined but *never exercised*.

This table is generated into `src/vr_render_ownership.inc` and compiled into
**both** the layer and the test suite, so the classifier cannot drift from the
shipped rig.

### The comment that was wrong

`BatchPrimaryClass` claimed "Same correction as MergedHalves" while using the
**opposite** order. Both were in fact *correct* — the merged rigs declare
hands L-then-R and the singles rig declares them R-then-L — but nothing was
checking, and the comment asserted an agreement that did not exist.

---

## 3. Laterality — settled structurally, unanimous

Idle pose, one common frame, six weapons covering all four required categories:

| weapon | kind | rear grip | support | inter-palm |
|---|---|---|---|---|
| kar98k | bolt rifle | **R** | L | 15.4 in |
| thompson | SMG | **R** | L | 11.9 in |
| m1garand | semi-auto rifle | **R** | L | 16.6 in |
| bar | unusual support layout (bipod LMG) | **R** | L | 15.5 in |
| sten | SMG, alternate hand rig | **R** | L | 13.8 in |
| mp40 | SMG | **R** | L | 12.1 in |

**RIGHT/TRIGGER = anatomical right (`bip01 r hand`) = the rear-grip palm.
LEFT/SUPPORT = `bip01 l hand` = the fore-end palm.** Unanimous, 6/6.

This confirms the 2026-08-28 mapping already in the source. **Why the earlier
sweep read backwards:** it named screen footprints by eye. Two forearms and two
hands at the bottom of a viewmodel frame are not distinguishable by position
without knowing the space, and the 2026-08-27 capture showed that space is the
Quake-view → GL-eye permutation, in which submitted +x is eye-*forward*, not
eye-right. Skin weights have no such ambiguity.

The mutant `own: MUTANT 5 — swapped LEFT/RIGHT ownership is REJECTED` states
this as geometry rather than as a claim.

---

## 4. Sleeves — the 2026-08-24 finding is superseded

`GLSurf_SleeveSplitIsAnatomical()` returned **0** ("two pieces of ONE arm"),
sourced from a pixel-footprint sweep: 71% overlap between the halves, IoU 0.38.

The skin weights say otherwise, and unambiguously: surface 2 is **120/120**
weighted under `bip01 r upperarm`, surface 3 is **120/120** under
`bip01 l upperarm`, with zero cross-arm vertices and zero seam triangles. **A
single arm's cloth cannot be skinned 100% to two different arm chains.**

The pixel metric could not have separated the hypotheses — two forearms overlap
on screen whichever arms they belong to. It is the same
screenshot-reading method that produced the laterality inversion twice.

**Changed:** the function returns 1; the sleeves are no longer hidden; each
takes its **own arm's** motion. Per the stereo rule, a sleeve does not copy its
hand's finished matrix (that matrix was built about one eye's palm pivot);
it rebuilds its own delta from the same *latched, eye-invariant* offset and
rotation, about a per-eye pivot — the arm's palm when that eye solved it this
scene, otherwise the sleeve's own centroid. No arm IK, which this milestone
does not require.

---

## 5. Type safety — the trap removed at the data model

`stable` was exactly `(trackState == ACTIVE || trackState == LOST)`: derivable,
never independently set, and its only unique contribution was colliding with
the enum beside it.

* **`stable` is deleted.** The illegal fixture state is now unrepresentable.
* **`trackState` is `enum class VRITrack : int`.** `someInt == VRI_TRACK_ACTIVE`
  no longer compiles anywhere in the tree.
* All decisions go through `VRI_HandTrackingActive()` /
  `VRI_HandTrackingLost()` / `VRI_HandTrackingUsable()`; `VRI_TrackCode()` is
  the explicit, named escape hatch for logging.
* Every consumer migrated: `vr_binds` (6 sites), `vr_weapon`,
  `vr_native_hands`, and all fixtures.
* `S_TrackStateTypeSafety` holds 4 `static_assert`s at compile time, walks the
  full state space, and runs the **mutant**: the old comparison is
  *unsatisfiable* over every state a producer can reach (fires 0/4) and
  disagrees with production on exactly the two engaging states.

---

## 6. Pair-level stereo eligibility — new

Each eye recovers the palm from its own draw, so the two can disagree about
whether a hand is recoverable — with any real refusal rate, about as often as a
coin. Acting on each answer as it arrives renders one eye's hand on the
controller and the other's on the weapon.

Native ownership is now a property of the **pair**:

* per-eye authored baselines (`GLSurf_GetHandBaselineEye`), plus a per-eye
  *participation* record (did this eye actually draw this class recently);
* comparison in **submitted space** — the frame both eyes share — so no
  assumption about whether the viewmodel modelview carries the inter-eye offset
  is needed. Tolerances 0.5 mm / 0.05°;
* a participating eye with no usable baseline **fails the pair for both eyes**;
* an eye that never draws the class imposes nothing (a monoscopic context —
  the flat probe, or the game before VR engages — must not be refused);
* enforced in `vr_glsurf` by a fail-closed gate, so a per-eye solve that
  happens to succeed can never carry one eye into native ownership alone.

The required mutant (eye 0 PASS / eye 1 FAIL) is `stereo/MUTANT`: gate closed,
reason `STEREO_DISAGREE`, **zero deltas reached either eye**, published pose
`ABSENT` rather than stale.

---

## 7. Coordinated, never parent/child

The gun's desired rear grip was computed from the **hand's target**
(`rh.targetEyeish`). The render matrices were independent, but the derivation
was not: the palm calibration silently dragged the weapon.

The weapon now has its **own** controller→rear-grip calibration
(`VRNH_SetGunCalibration`), anchored on the raw right grip. Both calibrations
default to zero against one grip frame, so the hand and the weapon coincide
exactly as before — but they are two independent parameters off one controller.
Measured: palm calibration alone moves the hand 1.47 u and the gun **0.000000
u**; gun calibration alone moves the weapon 1.86 u and the hand **0.000000 u**.
