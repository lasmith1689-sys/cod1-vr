# VBO validity contract, the 0.0455 m question, and temporal stereo — 2026-09-01

Offline cycle against the project-lead review of 2026-08-29. The game was
never launched, the headset was never launched, no attended run happened. The
SAFE baseline (`B3705FE5…` / `F7AB7A2D…`) was verified in the game folder at
the start of the cycle and stayed there until the gated deploy at the end.

---

## 1. The VBO shadow contract — now explicit and impossible to regress

`src/vr_vbo.{h,cpp}` tracks, per buffer: allocation generation (every
`BufferData`), content generation (every mirrored write), declared size, and
the **set of byte intervals written in the current allocation generation**
(up to 16 merged, sorted, disjoint spans). Everything outside those spans is
UNKNOWN.

| engine call | effect on the shadow |
|---|---|
| `BufferData(size, data)` | new allocation generation; `[0,size)` VALID |
| `BufferData(size, NULL)` | new allocation generation; **entire store UNKNOWN**; zero-filled only so a mistaken read is deterministic — never vouched for |
| `BufferSubData(off, len)` | exactly `[off, off+len)` VALID (merges with abutting spans) |
| `Map … Unmap` | whole store VALID (the mapping contract syncs the whole store) |
| span table full | the smallest span is dropped and counted (`validSpanDrops`): written bytes become refusals, never a false vouch |

Resolves report a reason: `OK`, `UNKNOWN_ID`, `STALE`, `RANGE`,
**`BYTES_UNKNOWN`** (with the unknown byte count and both generations).
A previous generation can never satisfy a read in a new one because
`BufferData` clears the span set before anything else happens.

**Refuse before the solver.** The palm and gun solves resolve their vertex
span *loosely* (allocated, in range, not stale) and hand the solver a read
gate (`VRHandPoseCanRead`). `SolveRigid` checks every landmark of a table
against the gate **before the first dereference**; a table with an unreadable
landmark is refused with `VRHP_REASON_BYTES_UNKNOWN` and Horn's method never
runs (`VRHandPoseStats::solverRuns` is the witness). Precedence when several
tables are tried: unknown bytes outrank a residual verdict — a residual next
to undefined bytes is not evidence. The refusal is a new **site 4** in
`GLSurfHandSolve` (`refusedUnknownBytes`, `lastRefReason`,
`lastRefUnknownBytes`, `lastRefGen/AllocGen`), distinct from `INDEX_RANGE`,
base/stale, `DEGENERATE` and `LANDMARK_RMS`. The gun reader goes through the
same gate (`VRGunPose_SolveByCountEx`, `GLSurfGunSolve::refusedUnknownBytes`).

Strict whole-span readers (pivots, the scratch transform) still refuse any
span containing an unknown byte.

Tests (`tests/vbo_contract_tests.inc`, all on the real VBO-backed merged hand
draw through `GLSurf_OnDrawElements`):

| case | result |
|---|---|
| A. orphan → landmark read | both hands refused, site 4 / `VBO_BYTES_UNKNOWN`, solver runs 0 → 0, palmtry `validRange=NO` |
| B. partial SubData missing the landmarks | refused; then writing only LEFT's landmark range: LEFT solves, RIGHT refused, solver ran **exactly once** |
| C. all landmark ranges written | both solve although the span still holds unknown filler bytes (per-landmark gating); a strict whole-span read still refuses |
| D. second orphan | allocation generation advances, previously good bytes UNKNOWN, the same draw refuses |
| E. old generation vs new | RIGHT valid only in generation 1 is refused in generation 2 while LEFT, rewritten, solves; the resolve reports `BYTES_UNKNOWN`, allocGen 2, exact unknown byte count |
| F. mixed valid/unknown cloud | refused as unknown bytes, not as a bad fit; solver runs unchanged; no residual exists |
| gun | an orphaned keyed batch refuses as `VBO_BYTES_UNKNOWN`, body solver never runs; solves once written |

---

## 2. 0.0448 vs 0.0455 — the deterministic sweep

`tools/orphan_residual_sweep.py` uses the **compiled** tables
(`src/vr_palm_landmarks.inc`, parsed from the C source), the exact runtime
residual (rotation-only Procrustes with the optimal translation, proper
rotation, rms over the table, divided by 39.3701), and the real shipped
geometry (thompson / `viewmodel_hands_new4`) posed through 22 animation
states, laid out byte-for-byte as the 2026-08-27 capture measured the packed
store (tex 8 B | normal 12 B | position 12 B per vertex; positions at byte
33480 = 1674 × 20, the recorded `0x82C8`).

The fingerprint is **two** facts: the logger printed `0.0455` (`%.4f`, so
[0.04545, 0.04555)) at every one of five heartbeats spanning ~3000 refusals
across varied gameplay — the residual was **constant** as well as equal to
0.0455. A pattern whose residual sweeps through the window with the animation
cannot have produced five identical readings.

| hypothesis | residual (m) | constant? |
|---|---|---|
| all 48 points zero (the orphan read) | **0.04484** TRIG / 0.04408 SUPP | yes |
| chunk boundary below the landmarks, rest zero | 0.04484 (= all zero) | yes |
| normal block read as positions | 0.03252 | yes |
| texcoord block read as positions (stride 8 / 12) | 0.04257 / 0.0419–0.0456 | yes / varies |
| stride-20 misread of the planar store | 0.046–0.049 | varies |
| position block read ±4/±8 bytes off | 0.008–0.029 | varies |
| contiguous prefix/suffix valid, rest zero (table order and vertex-index order, every k) | 0.03–0.07 | **varies** |
| subset current frame, rest previous frame (every k) | 0.00–0.22 | **varies** |
| half previous generation, half zero | 0.05–0.33 | varies |
| SubData chunk at every 4 KB boundary, rest previous frame | solves, or varies | — |

**Result: no constant pattern lands in the window; no constant pattern is
within 1% of 0.0455.** 101 partial-write patterns *cross* the window while
varying two- to ten-fold with animation — inconsistent with the five constant
readings. The nearest constant candidate remains the all-zero orphan read at
0.04484 m, 1.5% below the printed value.

**Verdict:** the orphan defect is real and fixed (section 1 makes the read
impossible), but **historical attribution stays unproven**. The upload
ordering evidence (every observed viewmodel buffer: `BufferData(NULL)` then
two or three abutting `SubData` chunks, then the draw, in program order)
argues *against* a draw ever landing between orphan and completion; the
constant residual argues for a rigid wrong point set rather than a partial
write. One observation decides it, and the instrumentation below takes it
during the normal Quest run: `palmtry … validRange=NO unknownBytes=N` would
have named the orphan; `shape 0.0000` names a degenerate cloud;
`shape == lastOk.shape` names a correspondence fault.

---

## 3. Temporal audit — the old gate was model B, incomplete; the new one is B + C

**Finding.** The 2026-08-29 `PairEligible` computed its verdict at
`VRNH_OnPairBegin` from `GLSurf_GetHandBaselineEye` — the previous pair's
per-eye solves. That is model B (decided from information known before eye
0) and uses no future information, so it was not logically invalid. But it
was *predictive*: if this pair's eye 1 solve failed for the first time, eye 0
had already rendered native and eye 1 fell back to vanilla — a half-native
pair was presented once and only the *following* pair closed. The
`stereo/MUTANT` test corrupted eye 1, drew it, then called the next
`OnPairBegin`, so it verified one-pair-late detection, not within-pair
prevention.

**Finding (section 5).** Its case C compared the two eyes' raw *authored*
baselines in submitted space and closed the gate when they differed. That was
conceptually wrong: the eyes render on consecutive game frames and the engine
advances the animation between them, so an authored difference is the normal
case. Worse, the hand target was a *relative* offset
(`final = authored + latchedOffset`), so an authored difference passed
straight through into the finals as false disparity of the animation's
per-frame motion.

### The replacement

* **Absolute targets.** `GLSurf_SetHandTarget` now takes the desired palm
  pose (head-midpoint eyeish frame). Each draw's delta is
  `Desired_eye × inverse(Authored_eye)` against **that draw's own fresh
  solve**, pivoting on that eye's own palm. Per-eye deltas differ; both eyes
  land on one pair-latched object. The gun already worked this way.
* **Eye offsets.** "Eye space" differs between the eyes by the IPD. vr.cpp
  now passes **both** located eye positions (`VRNH_OnPairBeginEyes`); the
  wiring installs each eye's own position in the midpoint frame
  (`GLSurf_SetEyeOffsets`), and the layer subtracts the current eye's offset
  from every absolute target — hands and gun. The monoscopic wrapper
  (`VRNH_OnPairBegin`) uses zero offsets, so the flat probe and fixtures are
  unchanged.
* **Per-eye apply records.** At the draw, the layer records per (object,
  eye): serial, drew, applied, and the *final* anchor/basis reconstructed
  from the matrix actually loaded (`GLSurf_GetEyeApply`). A transform that
  fails to reach GL clears `applied`.
* **The pre-submit verdict (model C).** `SubmitBothEyesAndEnd`, after the
  camera stereo gate and before `xrEndFrame`, calls `VRNH_PairPreSubmit` for
  the serial being submitted. For each of RIGHT, LEFT, GUN: an object one eye
  applied and the other drew but did not apply is **HALF_NATIVE**; an object
  both eyes applied is reconstructed from each eye's final pose into
  PairWorld (room metres, Quake axes, through that eye's own located
  position and the pair's head basis) and must agree within **0.5 mm /
  0.05°** or it is **DISAGREE**. A refused pair takes the existing abort
  path: the frame is ended with **zero layers** and counted as an aborted
  pair; the compositor reprojects the previous frame; the next boundary
  starts a fresh pair. The failing object is then held vanilla on both eyes
  for one pair (`PAIR_COOLDOWN`), so a persistent refusal degrades to
  alternating vanilla/abort rather than abort/abort.
* **The predictive gate stays** as a pre-filter (a participating eye with no
  fresh baseline closes the pair), minus the authored comparison.
* **Sleeves** take the same rigid delta their hand received in the same
  eye-scene; a sleeve whose hand did not solve stays vanilla.
* **Angle metric.** The pair angle is measured as
  `2·asin(‖R−I‖_F / 2√2)` in double. The acos-of-trace form the 2026-08-29
  code used has a float floor of ~0.03°; in the temporal soak it reported 13
  gun "disagreements" at 0.000 mm / 0.0560° between *identical* bases, which
  would have refused perfect pairs.

### Tests (`tests/temporal_tests.inc`), all through the production submit path with the mocked runtime and a 64 mm IPD

* **A, true order, animation advancing between the eyes:** after eye 0's
  frame the RIGHT record for eye 0 is applied under the pair serial and eye
  1 holds no record for it; the XR frame is still open. After eye 1: submitted
  with one projection layer; verdict OK; every object applied in both eyes;
  PairWorld agreement ≤ 0.5 mm / 0.05°; the two applied matrices differ; the
  two eye-space finals sit exactly the eye separation apart.
* **B, the mutant eye 0 PASS / eye 1 FAIL:** the runtime receives the frame
  with **zero layers** (`endsEmpty` +1, `endsWithLayers` unchanged,
  `lastEnd.layerCount == 0`); verdict `HALF_NATIVE` on R; counted by vr.cpp
  and as an aborted pair; the next pair is vanilla on both eyes
  (`PAIR_COOLDOWN`) and **is** submitted; the pair after resumes native on
  both eyes.
* **B′ (eye 0 FAIL / eye 1 PASS)** and **B″ (LEFT only fails):** refused;
  B″ attributes to LEFT with RIGHT OK and holds only LEFT afterwards.
* **C, the space mutant:** both eyes handed the same eye-space target →
  `DISAGREE` by 64.0 mm (the IPD), zero layers. Proves the eye offsets are
  load-bearing.
* The layer-level `stereo_tests.inc` case C now asserts the opposite of its
  2026-08-29 self: differing authored poses keep the gate open and the
  pre-submit verdict reconstructs both finals to one object; its half-native
  mutant is judged by `VRNH_PairPreSubmit`, not by the next boundary.

### The temporal soak (10,000 pairs, true order)

Schedule: per-frame animation advance between the eyes of every pair,
controller motion, head motion every 3 pairs, 25 game-turn events, R/L
tracking dropouts (469 pairs), 10 weapon switches, LEFT a metre away on 1391
pairs, injected solve refusals in eye 1 (104), eye 0 (43) and LEFT-only (67).

| count | value |
|---|---|
| pairs begun / eye 0 renders / eye 1 renders | 10000 / 10000 / 10000 |
| pairs submitted | 9789 |
| pairs aborted/fail-closed at pre-submit | 211 (half-native 211, **disagree 0**) |
| cooldown pairs | 353 |
| engaged R / L / gun | 9701 / 9468 / 9701 |
| transformed draws R / L / gun | 19256 / 18729 / 19402 |
| pairs with both eyes applied R / L / gun | 9555 / 9261 / 9701 |
| refusals: VBO-unknown R / L | 0 / 0 (client arrays in this fixture) |
| refusals: landmark RMS R / L | 147 / 214 (the injected ones) |
| half-native pairs **submitted** | **0** |
| NaN / stale-valid poses / ownership collisions / camera mutations / latch mismatches / re-entries / XR violations | 0 / 0 / 0 / 0 / 0 / 0 / 0 |

Every injected refusal on an engaged hand was refused at pre-submit and
nothing else was; the three injections that fell on dropout or cooldown pairs
had nothing to refuse.

---

## 4. Runtime diagnostics for the Quest run

`palmtry:` — the first 16 hand-solve attempts (layer ring, drained at the
pair boundary): pair, eye, owner, rig, vbo, content/allocation generation,
vertex and index offsets, span, `validRange=YES/NO`, `unknownBytes`,
`solve=PASS/REFUSE`, reason, site, rms, shape. Then silence.

`objapply:` — the first 10 judged pairs in which anything applied (and every
refused pair up to 30): per object, applied per eye, both eyes' final
PairWorld positions (metres), position/rotation disagreement, and
`pairStereoVerdict=OK|REJECT <reason>`.

Heartbeat (every ~600 pairs): refusals split `idx/base/solve/bytesUnknown`
for both hands with the last refusal's reason, generations and unknown byte
count; and `pairs judged / accepted / rejected (halfNative, disagree) /
cooldown / bothApplied R L gun`.

---

## 5. What the runtime does on a refused pair

`xrEndFrame` with `layerCount = 0` at the frame's predicted display time
(the same path every camera stereo fault and capture failure already used).
The compositor holds/reprojects the previous frame for one display interval.
No image of the refused pair reaches the headset. The pair is counted in
`abortedPairs` and in `VR_NativePairRejectCount()`; the log names the object
and reason for the first eight.

---

## 6. Gate, checkpoint, candidate

* Gate (2026-09-01): **1654 safe + 131 DLL = 1785 passed, 0 failed**, 47
  mutant assertions across the suites (three-object separation, laterality,
  stereo, temporal, VBO contract, type safety, right-stick).
* Source/build checkpoint: `checkpoints/candidate-6A221ECD-2026-09-01/`
  (src, tests, tools, docs, build.ps1, the gated DLL and cfg, SHA256SUMS over
  115 files). The safe checkpoint `deployed-as-found-2026-08-24` is untouched.
* Built DLL (`/Brepro`, reproduced byte-identical by the deploy rebuild):
  `6A221ECDD667D069AC43A74C3CA330F56A1DBE43983630A1D25CC205F3115FC1`.
* Candidate cfg `78CAC0BD…`: native hands ON, native two-hand ON, physical
  reload ON; `vr_aim_mode 0` (no fire-direction redirection), no
  `vr_probe_arm`, no capture, no autoquit (the deploy-time contract check
  reads the file and refuses an armed cfg).
