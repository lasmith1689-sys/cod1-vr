#!/usr/bin/env python3
"""Can the first-person hands model be split into a left half and a right half?

The whole independent-hands question turns on this. The rig is one skeleton
holding both arms and one animation drives all of it, so a second entity add
draws four arms -- that much was already established. What was NOT established
is whether the *geometry* is separable, because `xmodelsurfs` had never been
decoded: the previous pass could see that the file declares four surfaces and
that the skeleton has four arm bone-groups, and deliberately refused to claim
the correspondence on a hunch.

This decodes the container and answers it with numbers. For every surface of
every hand rig it recovers the vertex and triangle counts, the exact set of
bones that influence it, and the side each of those bones belongs to -- taken
from the flag byte and the parent chain, never from the bone name, because on
these rigs two bones are named `r` and parented to the LEFT hand.

Writes docs/hand_partition.json. Reads the installed pk3s; writes nothing else,
launches nothing, and extracts no mesh data -- only counts and classifications.

Usage:  python tools/extract_hand_partition.py [--json PATH] [--quiet]
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import codasset as CA  # noqa: E402

# The three first-person hand models. `viewmodel_hands` is shipped but no SP
# weapon selects it; it is analysed anyway so the conclusion is about the
# format and the rig family, not about one file.
HAND_MODELS = ("viewmodel_hands_new", "viewmodel_hands_sten", "viewmodel_hands")

LEFT_ONLY = "LEFT_ONLY"
RIGHT_ONLY = "RIGHT_ONLY"
MIXED = "MIXED"
UNSKINNED = "UNSKINNED"
UNKNOWN = "UNKNOWN"


def classify(sides):
    """Surface class from the set of sides its influencing bones carry."""
    if not sides:
        return UNSKINNED
    if sides == {CA.SIDE_LEFT}:
        return LEFT_ONLY
    if sides == {CA.SIDE_RIGHT}:
        return RIGHT_ONLY
    if CA.SIDE_LEFT in sides and CA.SIDE_RIGHT in sides:
        return MIXED
    return UNKNOWN


def vertex_side(vert, sides):
    """Side of one vertex, over every bone that influences it at all.

    A vertex counts as mixed if ANY influence crosses, however small the
    weight: a 1% bleed still ties the vertex to a bone on the other arm, and a
    split that ignores it produces a seam that stretches when the two hands
    separate. Weight thresholds are a rendering compromise, not a fact about
    the asset, so this reports the fact.
    """
    seen = set()
    for bone, _pos, _w in vert.weights:
        if 0 <= bone < len(sides):
            seen.add(sides[bone])
        else:
            seen.add(CA.SIDE_NEUTRAL)
    return classify(seen)


def analyse_model(fs, model_name):
    xm = CA.parse_xmodel(fs.read("xmodel/" + model_name), model_name)
    parts_name = xm.parts_name
    parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + parts_name), parts_name)
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + parts_name), parts_name)
    sides = CA.bone_sides(parts)

    out = {
        "model": model_name,
        "parts": parts_name,
        "bones": len(parts.bones),
        "bones_left": sides.count(CA.SIDE_LEFT),
        "bones_right": sides.count(CA.SIDE_RIGHT),
        "bones_neutral": sides.count(CA.SIDE_NEUTRAL),
        "materials": list(xm.materials),
        "surfaces": [],
    }

    for s in surfs.surfaces:
        bones = sorted(s.bones)
        bside = {}
        for b in bones:
            bside[b] = sides[b] if 0 <= b < len(sides) else CA.SIDE_NEUTRAL
        vclass = {LEFT_ONLY: 0, RIGHT_ONLY: 0, MIXED: 0, UNSKINNED: 0, UNKNOWN: 0}
        for v in s.verts:
            vclass[vertex_side(v, sides)] += 1
        # A triangle crosses the partition when its three vertices do not
        # agree on a side. This is the number that decides whether a split can
        # be done by dropping whole triangles.
        crossing = 0
        vsides = [vertex_side(v, sides) for v in s.verts]
        for a, b, c in s.tris:
            t = {vsides[a], vsides[b], vsides[c]}
            if MIXED in t or len(t - {UNSKINNED}) > 1:
                crossing += 1
        # Hand-level or arm-level. The flag byte's 0x2 bit is set at the hand
        # and everything below it, so a surface whose influences are ALL
        # hand-level is a hand+fingers shell, and one that reaches a forearm
        # or an upperarm is the sleeve. This is what makes "hands only" a
        # selectable rendering option rather than a modelling job.
        arm = any((parts.bones[b].flag & CA.BONE_FLAG_HAND) == 0
                  for b in bones if 0 <= b < len(parts.bones))
        out["surfaces"].append({
            "part": "ARM" if arm else "HAND",
            "index": s.index,
            "material": (xm.materials[s.index]
                         if s.index < len(xm.materials) else None),
            "verts": s.num_verts,
            "tris": s.num_tris,
            "tris_rebuilt": len(s.tris),
            "strips": len(s.strips),
            "physiqued": s.physiqued,
            "rigid_bone": s.rigid_bone,
            "rigid_verts": s.num_rigid_verts,
            "weight_records": s.num_weight_recs,
            "bones": bones,
            "bone_names": [parts.bones[b].name for b in bones
                           if 0 <= b < len(parts.bones)],
            "bone_sides": {str(k): v for k, v in bside.items()},
            "class": classify(set(bside.values()) - {CA.SIDE_NEUTRAL}),
            "vertex_class": vclass,
            "tris_crossing": crossing,
            "bytes": s.byte_end - s.byte_start,
        })

    tot_v = sum(x["verts"] for x in out["surfaces"])
    tot_t = sum(x["tris"] for x in out["surfaces"])
    left_v = sum(x["verts"] for x in out["surfaces"] if x["class"] == LEFT_ONLY)
    right_v = sum(x["verts"] for x in out["surfaces"] if x["class"] == RIGHT_ONLY)
    mixed_v = sum(x["verts"] for x in out["surfaces"] if x["class"] == MIXED)
    out["totals"] = {
        "verts": tot_v,
        "tris": tot_t,
        "verts_left_only": left_v,
        "verts_right_only": right_v,
        "verts_mixed": mixed_v,
        "pct_left_only": 100.0 * left_v / tot_v if tot_v else 0.0,
        "pct_right_only": 100.0 * right_v / tot_v if tot_v else 0.0,
        "pct_mixed": 100.0 * mixed_v / tot_v if tot_v else 0.0,
        "tris_crossing": sum(x["tris_crossing"] for x in out["surfaces"]),
        "separable": (mixed_v == 0
                      and sum(x["tris_crossing"] for x in out["surfaces"]) == 0
                      and left_v + right_v == tot_v),
    }
    return out


def sweep_all(fs):
    """Structural check over every shipped xmodelsurfs, not just the hands."""
    paths = fs.list("xmodelsurfs/")
    ok = 0
    bad = []
    nphys = nrigid = 0
    tri_ok = tri_bad = 0
    for p in paths:
        nm = p.split("/", 1)[1]
        try:
            m = CA.parse_xmodelsurfs(fs.read(p), nm, verts=False)
        except Exception as e:
            bad.append((nm, str(e)[:100]))
            continue
        ok += 1
        for s in m.surfaces:
            if s.physiqued:
                nphys += 1
            else:
                nrigid += 1
    # Rebuilding triangles is the expensive half, so it runs over a slice
    # large enough to be a real test and small enough to keep the gate quick.
    for p in paths[::7]:
        nm = p.split("/", 1)[1]
        try:
            m = CA.parse_xmodelsurfs(fs.read(p), nm)
        except Exception:
            continue
        for s in m.surfaces:
            if len(s.tris) == s.num_tris:
                tri_ok += 1
            else:
                tri_bad += 1
    return {
        "files": len(paths), "exact_eof": ok, "failed": bad,
        "surfaces_physiqued": nphys, "surfaces_rigid": nrigid,
        "tri_count_reproduced": tri_ok, "tri_count_wrong": tri_bad,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--json", default=os.path.join(HERE, os.pardir, "docs",
                                                   "hand_partition.json"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    doc = {"models": [], "sweep": sweep_all(fs)}
    for nm in HAND_MODELS:
        if "xmodel/" + nm not in fs:
            continue
        doc["models"].append(analyse_model(fs, nm))
    fs.close()

    if not args.quiet:
        s = doc["sweep"]
        print("xmodelsurfs sweep : %d/%d files consume to exact EOF "
              "(%d physiqued + %d rigid surfaces)"
              % (s["exact_eof"], s["files"], s["surfaces_physiqued"],
                 s["surfaces_rigid"]))
        print("                    %d/%d sampled surfaces reproduce their "
              "declared triangle count"
              % (s["tri_count_reproduced"],
                 s["tri_count_reproduced"] + s["tri_count_wrong"]))
        for f in s["failed"][:10]:
            print("    FAILED %s: %s" % f)
        for m in doc["models"]:
            print()
            print("=== %s (%s, %d bones: %dL %dR %d neutral) ==="
                  % (m["model"], m["parts"], m["bones"], m["bones_left"],
                     m["bones_right"], m["bones_neutral"]))
            print("  %-3s %-11s %-5s %6s %6s %7s  %s"
                  % ("#", "class", "part", "verts", "tris", "cross", "material"))
            for x in m["surfaces"]:
                print("  %-3d %-11s %-5s %6d %6d %7d  %s"
                      % (x["index"], x["class"], x["part"], x["verts"],
                         x["tris"], x["tris_crossing"], x["material"]))
            t = m["totals"]
            print("  left-only %.1f%%  right-only %.1f%%  mixed %.1f%%  "
                  "crossing triangles %d  -> separable: %s"
                  % (t["pct_left_only"], t["pct_right_only"], t["pct_mixed"],
                     t["tris_crossing"], "YES" if t["separable"] else "NO"))

    path = os.path.abspath(args.json)
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
    if not args.quiet:
        print()
        print("wrote %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
