#!/usr/bin/env python
"""Systematic search for the geometric mismatch that yields the runtime's
CONSTANT 0.0455 m palm-solve residual (project-lead item 2, hypotheses D/G/H/I).

The runtime refusal is constant across thousands of draws and across gameplay,
which means the observed landmark cloud is RIGID with respect to the bind
cloud -- a varying residual would mean a non-rigid (finger-influenced) read.
So the search space is: which rigid cloud is being read instead of the palm's?

Cells tested
  * every vertex-base offset k in a wide window (alternate array base)
  * every other surface of the same rig read at the table's local indices
  * the degenerate cases (all-zero / constant buffer content)
  * a foreign model's vertices (a shared stream buffer holding someone else)

A cell qualifies only if it is CONSTANT across animation states (max-min
below a tight bound) AND lands on 0.0455 +/- 0.0005.
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space
from verify_palm_solve import kabsch, skin
from crosssolve_palm import solve_rms, gyration, load_rig, hand_entry, bind_of

UNITS_PER_M = 39.3701
TARGET = 0.0455
TOL = 0.0006
FLAT = 0.0004          # max-min below this counts as "constant"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--landmarks", default="docs/palm_landmarks.json")
    ap.add_argument("--weapon", default="thompson")
    ap.add_argument("--rig", default="viewmodel_hands_new")
    args = ap.parse_args()

    lm = json.load(open(args.landmarks))
    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)
    live = load_rig(fs, assets, lm, args.rig)

    wraw = fs.read("weapons/sp/" + args.weapon)
    parts_txt = wraw.decode("latin-1").split(chr(92))
    fields = {parts_txt[i]: parts_txt[i + 1]
              for i in range(1, len(parts_txt) - 1, 2)}
    gun_parts, _ = assets.parts_for(fields["gunModel"])
    bones, index, _ = merge_skeletons(live["parts"], gun_parts)

    states = []
    for fld in ("idleAnim", "fireAnim", "adsUpAnim", "reloadAnim"):
        nm = fields.get(fld, "")
        anim = assets.anim(nm) if nm else None
        if anim is None:
            continue
        n = anim.num_frames
        for fr in sorted(set([0, n // 3, (2 * n) // 3, max(0, n - 1)])):
            states.append((anim, fr))
    print("rig %s   weapon %s   states %d" % (args.rig, args.weapon, len(states)))

    # The merged submitted array, as the engine lays it out: surfaces
    # concatenated in file order.  Absolute vertex index -> (surface, local).
    surfs = live["surfs"].surfaces
    layout = []
    for si, su in enumerate(surfs):
        for li in range(len(su.verts)):
            layout.append((si, li))
    print("submitted array: %d verts over %d surfaces %s"
          % (len(layout), len(surfs), [len(s.verts) for s in surfs]))

    posed_cache = {}

    def posed_at(k):
        if k not in posed_cache:
            anim, fr = states[k]
            posed_cache[k] = pose_model_space(bones, [(anim, fr)])
        return posed_cache[k]

    def cloud(abs_indices, k):
        """Skin the vertices at these ABSOLUTE array indices, one anim state."""
        posed = posed_at(k)
        meta = []
        for ai in abs_indices:
            if ai < 0 or ai >= len(layout):
                return None
            si, li = layout[ai]
            meta.append(surfs[si].verts[li].weights)
        return skin(meta, posed, index)

    SIDES = {"TRIGGER": 1, "SUPPORT": 0}
    rows = []

    for side_name, hidx in SIDES.items():
        h = hand_entry(live, hidx)
        base = h["vertexBase"]
        abs_idx = [base + l["localIndex"] for l in h["landmarks"]]
        bind = bind_of(live, hidx)
        gyr = gyration(bind)

        # --- degenerate: the shadow vouched for zero / constant content -----
        rows.append(("%s degenerate(zero/const buffer)" % side_name,
                     gyr, gyr, "constant by construction"))

        # --- base-offset sweep ---------------------------------------------
        for k in range(-1000, 1001):
            if k == 0:
                continue
            shifted = [a + k for a in abs_idx]
            if min(shifted) < 0 or max(shifted) >= len(layout):
                continue
            vals = []
            bad = False
            for s in range(len(states)):
                o = cloud(shifted, s)
                if o is None:
                    bad = True
                    break
                vals.append(solve_rms(bind, o))
            if bad or not vals:
                continue
            lo, hi = min(vals), max(vals)
            if hi - lo <= FLAT and abs(sum(vals) / len(vals) - TARGET) <= TOL:
                rows.append(("%s base-offset k=%+d" % (side_name, k),
                             lo, hi, "CONSTANT + ON TARGET"))

        # --- other surfaces at the table's LOCAL indices --------------------
        for si, su in enumerate(surfs):
            locs = [l["localIndex"] for l in h["landmarks"]]
            if max(locs) >= len(su.verts):
                continue
            cum = sum(len(surfs[j].verts) for j in range(si))
            vals = []
            for s in range(len(states)):
                o = cloud([cum + l for l in locs], s)
                if o is None:
                    break
                vals.append(solve_rms(bind, o))
            if len(vals) != len(states):
                continue
            lo, hi = min(vals), max(vals)
            flag = ("CONSTANT + ON TARGET"
                    if (hi - lo <= FLAT and abs(sum(vals) / len(vals) - TARGET) <= TOL)
                    else ("constant" if hi - lo <= FLAT else "varies"))
            rows.append(("%s localIdx on surface %d" % (side_name, si),
                         lo, hi, flag))

    print("\n%-46s %10s %10s  %s" % ("CELL", "minRMS_m", "maxRMS_m", "note"))
    print("-" * 92)
    onTarget = []
    for name, lo, hi, note in rows:
        mark = ""
        if abs((lo + hi) / 2 - TARGET) <= TOL and (hi - lo) <= FLAT:
            mark = "  <<<"
            onTarget.append(name)
        print("%-46s %10.5f %10.5f  %s%s" % (name, lo, hi, note, mark))

    print("\ntarget = runtime constant %.4f m (+/- %.4f)" % (TARGET, TOL))
    print("cells that are CONSTANT and ON TARGET: %s"
          % (", ".join(onTarget) if onTarget else "NONE"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
