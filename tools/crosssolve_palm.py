#!/usr/bin/env python
"""Offline cross-solve matrix for the ~50% palm refusal (project-lead item 2).

WHAT THIS ANSWERS
-----------------
The 2026-08-28 run refused ~half of every hand-solve attempt at the SOLVER
site with a constant best residual of 0.0455 m, on a draw whose shape
(idx=7824, span=1674) is the merged hand batch and nothing else.  A refusal
means EVERY table VRHandPose_SolveAny tries -- the installed default plus each
shipped rig's table for that hand -- missed the residual gate (0.004 m).

This script reproduces the solver offline against the real shipped geometry and
runs the hypothesis matrix the brief lists, so the runtime residual can be
matched to a specific geometric mismatch by FINGERPRINT rather than argument.

Every cell reports the residual the production Kabsch would produce.  A cell
landing on 0.0455 m identifies the mismatch.
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

UNITS_PER_M = 39.3701
GATE_M = 0.004


def rms(bind, obs, r, t):
    """Residual of the rigid fit, in the units `obs` is in."""
    s = 0.0
    for pb, po in zip(bind, obs):
        x = r[0][0] * pb[0] + r[0][1] * pb[1] + r[0][2] * pb[2] + t[0]
        y = r[1][0] * pb[0] + r[1][1] * pb[1] + r[1][2] * pb[2] + t[1]
        z = r[2][0] * pb[0] + r[2][1] * pb[1] + r[2][2] * pb[2] + t[2]
        s += (x - po[0]) ** 2 + (y - po[1]) ** 2 + (z - po[2]) ** 2
    return math.sqrt(s / len(bind))


def solve_rms(bind, obs):
    """Production-equivalent: rotation-only Kabsch, residual in metres."""
    if len(bind) != len(obs) or len(bind) < 3:
        return None
    r, t = kabsch(bind, obs)
    return rms(bind, obs, r, t) / UNITS_PER_M


def gyration(obs):
    """VRHandPoseSample::shape -- rms distance from the cloud's own centroid."""
    n = len(obs)
    c = [sum(p[i] for p in obs) / n for i in range(3)]
    s = sum((p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2
            for p in obs)
    return math.sqrt(s / n)


def load_rig(fs, assets, lm, model_name):
    """Everything needed to pose one hands rig and read its landmarks."""
    rig = next((r for r in lm["rigs"] if r.get("model", "").lower()
                == model_name.lower()), None)
    if rig is None:
        return None
    hand_parts, hbase = assets.parts_for(rig["model"])
    if hand_parts is None or not hbase:
        return None
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + hbase), hbase)
    return {"rig": rig, "parts": hand_parts, "surfs": surfs, "base": hbase}


def hand_entry(rig, side):
    """side: 0 = the table the runtime calls TRIGGER, 1 = SUPPORT."""
    return rig["rig"]["hands"][side]


def observed(rigdef, side, bones, index, anim, frame):
    """Skinned landmark positions for one hand of one rig at one anim frame,
    in ENGINE UNITS -- exactly what the runtime reads out of the vertex array."""
    h = hand_entry(rigdef, side)
    surf = rigdef["surfs"].surfaces[h["surface"]]
    meta = [surf.verts[l["localIndex"]].weights for l in h["landmarks"]]
    posed = pose_model_space(bones, [(anim, frame)])
    return skin(meta, posed, index)


def bind_of(rigdef, side):
    h = hand_entry(rigdef, side)
    return [tuple(c * UNITS_PER_M for c in l["bindM"]) for l in h["landmarks"]]


def mirror(pts, axis):
    return [tuple(-p[i] if i == axis else p[i] for i in range(3)) for p in pts]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--landmarks", default="docs/palm_landmarks.json")
    ap.add_argument("--weapon", default="thompson")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    lm = json.load(open(args.landmarks))
    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)

    wraw = fs.read("weapons/sp/" + args.weapon)
    parts_txt = wraw.decode("latin-1").split(chr(92))
    fields = {parts_txt[i]: parts_txt[i + 1]
              for i in range(1, len(parts_txt) - 1, 2)}
    hand_model = fields["handModel"].replace("\\", "/").split("/")[-1].lower()

    rigs = {}
    for r in lm["rigs"]:
        got = load_rig(fs, assets, lm, r["model"])
        if got:
            rigs[r["model"]] = got
    print("rigs loaded: %s" % ", ".join(sorted(rigs)))
    print("weapon %s uses handModel %s" % (args.weapon, hand_model))

    live = rigs.get(hand_model)
    if live is None:
        print("FAIL: weapon's hand rig not in landmark file")
        return 1

    gun_parts, _ = assets.parts_for(fields["gunModel"])
    bones, index, _ = merge_skeletons(live["parts"], gun_parts)

    # A spread of animation states: idle, mid-fire, ADS, reload (hand open and
    # closed).  If the refusal were animation-driven this is where it shows.
    states = []
    for fld in ("idleAnim", "fireAnim", "adsUpAnim", "reloadAnim",
                "reloadEmptyAnim", "meleeAnim"):
        nm = fields.get(fld, "")
        if not nm:
            continue
        anim = assets.anim(nm)
        if anim is None:
            continue
        n = anim.num_frames
        for fr in sorted(set([0, n // 3, (2 * n) // 3, max(0, n - 1)])):
            states.append((fld, fr, anim))
    print("animation states sampled: %d" % len(states))

    results = {}

    def record(tag, vals):
        vals = [v for v in vals if v is not None]
        if not vals:
            return
        results[tag] = {"n": len(vals), "min": min(vals), "max": max(vals),
                        "mean": sum(vals) / len(vals)}

    SIDES = {0: "TRIGGER", 1: "SUPPORT"}

    # ---- baseline: correct table on correct geometry ---------------------
    for side in (0, 1):
        b = bind_of(live, side)
        vals = []
        for fld, fr, anim in states:
            o = observed(live, side, bones, index, anim, fr)
            if o:
                vals.append(solve_rms(b, o))
        record("A_correct_%s" % SIDES[side], vals)

    # ---- B: cross-hand -- one hand's table against the other's geometry --
    for side in (0, 1):
        other = 1 - side
        b = bind_of(live, side)
        vals = []
        for fld, fr, anim in states:
            o = observed(live, other, bones, index, anim, fr)
            if o and len(o) == len(b):
                vals.append(solve_rms(b, o))
        record("B_crosshand_%stable_on_%sgeom" % (SIDES[side], SIDES[other]),
               vals)

    # ---- C: cross-rig -- another rig's table on this geometry ------------
    for other_name, other in rigs.items():
        if other_name == hand_model:
            continue
        for side in (0, 1):
            b = bind_of(other, side)
            vals = []
            for fld, fr, anim in states:
                o = observed(live, side, bones, index, anim, fr)
                if o and len(o) == len(b):
                    vals.append(solve_rms(b, o))
            record("C_crossrig_%s_%s_on_%s" % (other_name, SIDES[side],
                                               hand_model), vals)

    # ---- D: mirrored geometry (chirality) --------------------------------
    for side in (0, 1):
        b = bind_of(live, side)
        for ax, axn in ((0, "x"), (1, "y"), (2, "z")):
            vals = []
            for fld, fr, anim in states:
                o = observed(live, side, bones, index, anim, fr)
                if o:
                    vals.append(solve_rms(b, mirror(o, ax)))
            record("D_mirror%s_%s" % (axn, SIDES[side]), vals)

    # ---- E: index shift -- landmarks read one half away ------------------
    # The merged batch's two halves are 837 apart; a table read at the wrong
    # base lands on the other hand's corresponding vertices.
    for side in (0, 1):
        b = bind_of(live, side)
        other = 1 - side
        h_self = hand_entry(live, side)
        h_other = hand_entry(live, other)
        surf_o = live["surfs"].surfaces[h_other["surface"]]
        nlocal = len(surf_o.verts)
        meta = []
        ok = True
        for l in h_self["landmarks"]:
            li = l["localIndex"]
            if li >= nlocal:
                ok = False
                break
            meta.append(surf_o.verts[li].weights)
        if not ok:
            continue
        vals = []
        for fld, fr, anim in states:
            posed = pose_model_space(bones, [(anim, fr)])
            o = skin(meta, posed, index)
            if o:
                vals.append(solve_rms(b, o))
        record("E_sameLocalIdx_otherSurface_%s" % SIDES[side], vals)

    # ---- F: stale half -- half the cloud from a different anim frame -----
    for side in (0, 1):
        b = bind_of(live, side)
        vals = []
        for i in range(len(states) - 1):
            f1, r1, a1 = states[i]
            f2, r2, a2 = states[i + 1]
            o1 = observed(live, side, bones, index, a1, r1)
            o2 = observed(live, side, bones, index, a2, r2)
            if o1 and o2:
                half = len(o1) // 2
                vals.append(solve_rms(b, o1[:half] + o2[half:]))
        record("F_splitframe_%s" % SIDES[side], vals)

    # ---- shape signatures (mirror-invariant; the runtime prints these) ---
    shapes = {}
    for side in (0, 1):
        vals = []
        for fld, fr, anim in states:
            o = observed(live, side, bones, index, anim, fr)
            if o:
                vals.append(gyration(o) / UNITS_PER_M)
        if vals:
            shapes[SIDES[side]] = {"min": min(vals), "max": max(vals)}

    print("\n%-52s %6s %10s %10s %10s  %s"
          % ("HYPOTHESIS", "n", "minRMS_m", "meanRMS_m", "maxRMS_m", "verdict"))
    print("-" * 110)
    hits = []
    for tag in sorted(results):
        v = results[tag]
        near = abs(v["mean"] - 0.0455) < 0.0025 or abs(v["min"] - 0.0455) < 0.0025
        passes = v["max"] < GATE_M
        verdict = "SOLVES" if passes else ("<<< 0.0455 MATCH" if near
                                           else "refuses")
        if near:
            hits.append(tag)
        print("%-52s %6d %10.5f %10.5f %10.5f  %s"
              % (tag, v["n"], v["min"], v["mean"], v["max"], verdict))

    print("\nshape signature (radius of gyration, m) -- mirror-INVARIANT:")
    for k, v in shapes.items():
        print("  %-10s %.6f .. %.6f" % (k, v["min"], v["max"]))

    print("\nruntime refusal fingerprint: bestRms 0.0455 m, gate %.3f m" % GATE_M)
    print("cells matching the runtime fingerprint: %s"
          % (", ".join(hits) if hits else "NONE"))

    if args.json_out:
        json.dump({"results": results, "shapes": shapes, "hits": hits,
                   "weapon": args.weapon, "handModel": hand_model},
                  open(args.json_out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
