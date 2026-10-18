# The native/dead-centre hand flicker: root cause (2026-09-03)

Hardware run: candidate `6A221ECD…`, log preserved at
`logs/hw-2026-09-03/codvr_result.log`. User report: hands and gun follow the
controllers but flicker back to the vanilla dead-centre viewmodel; hands not
aligned to the physical hand; shots go straight ahead.

## 1. What the log shows (pairs 987–1005, the first hand movement)

Every `palmtry` line has `validRange=YES unknownBytes=0`: the VBO shadow
vouched for every landmark byte on every attempt. **VBO_BYTES_UNKNOWN never
fired** (0 of 6426 attempts per hand by pair 4200). The 2026-09-01 orphan
hypothesis for the historical refusals is therefore refuted by direct
observation.

Refusals are all `LANDMARK_RMS site 3` with a constant fingerprint:

| hand | refused rms | refused gyration | healthy gyration |
|---|---|---|---|
| RIGHT (TRIG table) | 0.0455 m | 1.9397 u | 1.7652 u |
| LEFT (SUPP table) | 0.0655 m | 2.6618 u | 1.7352 u |

Pair timeline: 987 eye0 PASS/PASS, eye1 REFUSE/REFUSE; 988 both eyes
REFUSE; 989–995 all PASS; 996 eye0 applied / eye1 not → `HALF_NATIVE`,
aborted. Refusals hit ~45 % of scene frames (2831 of 6426 by pair 4200),
on both the 7824-index merged draw (2162) and the 3912-index single draws
(669). Both hands refuse together, on the same game frame.

## 2. The raw sample decides it

The log kept the raw refused landmark positions (`palmdump TRIG REFUSED`):
every RIGHT-table vertex reads at x ≈ 16–21 in, where the healthy LEFT palm
sits (the healthy RIGHT palm is at x ≈ 1–6 in). And exactly one index is in
both tables' local ranges: refused RIGHT vertex **909** = (18.557 −2.075
−7.959) = healthy LEFT vertex **72**, and refused LEFT vertex 72 = (3.370
−3.933 −7.666) = healthy RIGHT vertex 909. 909 − 837 = 72.

**On the refused frames the engine packs the two hand surfaces in the
opposite order**: surface 1 (`bip01 r hand`, the RIGHT hand) at vertices
[0, 837) and surface 0 (LEFT) at [837, 1674). The landmark tables, the
`MergedHalves` split classes and the single-draw ordinal classes all assume
the file order, so on those frames the RIGHT table reads the LEFT hand's
shell and vice versa. A rigid solve against the wrong hand refuses with a
constant residual — the fingerprint.

## 3. Offline reproduction (tools/swap_order_proof.py)

Posing the shipped rigs through their idle animation and packing the merged
batch with the surfaces swapped reproduces every number to the printed
digit for the weapon the log shows the user carrying (BAR, body surface 516):

| | normal order | swapped order |
|---|---|---|
| TRIG rms / gyration | 0.0000 m / 1.7652 u | **0.0455 m / 1.9397 u** |
| SUPP rms / gyration | 0.0000 m / 1.7352 u | **0.0655 m / 2.6618 u** |

The other rigs/weapons give 0.0433–0.0466 m for the swapped RIGHT read, so
the 2026-08-28 run's constant 0.0455 was the same defect on the same weapon.
Historical attribution: **proven — surface order, not VBO validity**.

## 4. Why the user sees a flicker

A refused solve leaves the hand exactly where the engine drew it (vanilla,
dead centre, head-coupled). Both hands refuse on the same frame, so the
pair is *coherently* vanilla in both eyes and the pre-submit verdict
accepts it (nothing applied, nothing to judge). Between such pairs the
native pairs are accepted too. The eye sees native → vanilla → native at
~50 % duty: the reported flicker. Genuinely mixed pairs (one eye normal,
one swapped) were correctly refused at pre-submit (121 of 3213 judged),
never submitted; the cooldown that follows each adds vanilla pairs.

So of the brief's mechanisms: **A** (solve alternates PASS/FAIL) caused by
**E** (surface order changes between draw variants). Not B/C (no mixed pair
was submitted), not D (no duplicate draw: one hand draw per scene), not F/G.

## 5. The fix (this cycle)

Hand identity is decided from the vertex content, never from draw order:
the solver tries each landmark table in both half-orders, the merged split
takes its half classes from which order solved, single-surface hand draws
are identified by which table solves, and sleeves are attributed to the
palm their cuff vertices are nearest. Draw order is only a fail-closed
fallback when nothing solves. See NATIVE-HANDS-PHASE-STATUS.md for the
counters and the policy that keeps a vanilla pair from being shown while
native ownership is healthy.

Offline evidence (tests/surface_order_tests.inc, temporal_tests.inc):

| case | result |
|---|---|
| swapped batch, solve only | both hands solve, `order=swapped`, 0 refusals |
| MUTANT: file-order-only solver on the swapped batch | both refuse `LANDMARK_RMS` site 3, wrong-hand residual centimetres |
| engaged, order alternating and mixed between the eyes, 12 pairs | 12/12 pairs OK, both hands applied in both eyes, RIGHT on the RIGHT controller every pair, 0 refusals |
| single-surface draws, either half, either order | 4/4 identified by content |
| merged sleeves, either order | each half took its own arm's delta, 4/4 |
| duplicate owner (native then unrecoverable copy; vanilla then native) | copy not drawn / drawn vanilla, pair judged; 8 prevented |
| 10k-pair temporal soak, surface order swapped on 10000 of 20000 frames | 0 half-native submitted, 0 duplicates, 0 disagreements, RMS refusals = injected corruption only |

Hardware truth still to be observed: the ownership counters and `order
swapped` in the next normal Quest run's `ownership:` heartbeat.
