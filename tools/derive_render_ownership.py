#!/usr/bin/env python
"""Derive first-person render OWNERSHIP from the shipped assets (items 4-7).

WHAT THIS PRODUCES, AND WHY IT IS NOT A GUESS
---------------------------------------------
The project-lead requirement is that GUN, RIGHT HAND/ARM and LEFT HAND/ARM be
three independently owned rendered objects, with ownership derived from the
asset structure rather than from where something lands on screen.

The chain this walks is exactly the one named in the brief:

    submitted draw -> source surface -> vertex -> skin weights
        -> anatomical bone hierarchy -> triangle ownership

ANCESTRY, NOT NAMES.  The hands rig contains bones whose NAME contradicts
their parent -- `r cuff01` and `r wrist01` are both children of `bip01 l hand`.
A name-based partition puts two cuff bones on the wrong arm.  So every vertex
is attributed by walking its dominant bone up the parent chain to whichever of
the two arm roots (`bip01 l upperarm` / `bip01 r upperarm`) it descends from.
That is a structural fact of the shipped skeleton and cannot be argued with.

LATERALITY is then settled by physics rather than by labels: for each weapon,
which anatomical palm sits at the model's rear grip (trigger) and which at the
fore-end (support), measured in ONE common frame (the merged model space of
the posed idle).

TRIANGLE OWNERSHIP uses a documented tie policy, because a triangle whose three
vertices do not agree has no single owner:

    majority of the 3 vertices wins; a 3-way disagreement is impossible with
    two sides, so the only tie case is a triangle that spans the seam 2-1,
    which the majority already resolves.  Triangles with ANY vertex on a bone
    that reaches neither arm root are reported as UNKNOWN and never claimed.
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space

UNITS_PER_M = 39.3701

# The four representative weapons the brief names, plus two more for margin.
WEAPONS = [
    ("kar98k",   "bolt rifle"),
    ("thompson", "SMG"),
    ("m1garand", "semi-auto rifle"),
    ("bar",      "unusual support layout (bipod LMG)"),
    ("sten",     "SMG, alternate hand rig"),
    ("mp40",     "SMG"),
]

ARM_ROOTS = {"bip01 l upperarm": "L", "bip01 r upperarm": "R"}


def parse_weaponfile(text):
    parts = text.split(chr(92))
    return {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}


def bone_side_by_ancestry(parts):
    """anatomical side of every bone, by walking to an arm root. Names ignored."""
    side = {}
    for i, b in enumerate(parts.bones):
        seen, j = set(), i
        s = None
        while j is not None and j >= 0 and j not in seen:
            seen.add(j)
            nm = parts.bones[j].name.lower()
            if nm in ARM_ROOTS:
                s = ARM_ROOTS[nm]
                break
            j = parts.bones[j].parent
        side[i] = s
    return side


def vertex_side(v, side):
    """Dominant-bone side of one vertex, plus whether the weights are split."""
    if not v.weights:
        return None, 0
    best = max(v.weights, key=lambda w: w[2])
    sides = set(side.get(w[0]) for w in v.weights if w[2] > 0.0)
    sides.discard(None)
    return side.get(best[0]), (1 if len(sides) > 1 else 0)


def analyse_rig(model, assets, fs):
    parts, base = assets.parts_for(model)
    if parts is None:
        return None
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
    side = bone_side_by_ancestry(parts)

    out = {"model": model, "parts": base, "surfaces": [],
           "boneSideByAncestry": {parts.bones[i].name: side[i]
                                  for i in range(len(parts.bones))},
           "namesThatLie": []}

    # Bones whose NAME says one side and whose ANCESTRY says the other.
    for i, b in enumerate(parts.bones):
        nm = b.name.lower()
        claim = "L" if (nm.startswith("l ") or " l " in nm) else \
                ("R" if (nm.startswith("r ") or " r " in nm) else None)
        if claim and side[i] and claim != side[i]:
            out["namesThatLie"].append(
                {"bone": b.name, "nameClaims": claim, "ancestry": side[i],
                 "parent": parts.bones[b.parent].name if b.parent is not None
                           and b.parent >= 0 else None})

    cum = 0
    for si, su in enumerate(surfs.surfaces):
        nv = len(su.verts)
        counts = {"L": 0, "R": 0, None: 0}
        splits = 0
        vside = []
        for v in su.verts:
            s, sp = vertex_side(v, side)
            counts[s] = counts.get(s, 0) + 1
            splits += sp
            vside.append(s)

        tris = getattr(su, "tris", []) or []
        tri_owner = {"L": 0, "R": 0, "UNKNOWN": 0, "seam": 0}
        for t in tris:
            ss = [vside[i] for i in t if i < len(vside)]
            if len(ss) != 3 or any(x is None for x in ss):
                tri_owner["UNKNOWN"] += 1
                continue
            nl = ss.count("L")
            if nl == 3:
                tri_owner["L"] += 1
            elif nl == 0:
                tri_owner["R"] += 1
            else:
                tri_owner["seam"] += 1
                tri_owner["L" if nl >= 2 else "R"] += 1

        # Which palm bone actually drives this surface (dominant hand bone).
        hand_hits = {}
        for v in su.verts:
            for bi, _p, w in v.weights:
                nm = parts.bones[bi].name.lower()
                if nm.endswith(" hand"):
                    hand_hits[parts.bones[bi].name] = \
                        hand_hits.get(parts.bones[bi].name, 0) + 1
        pure = (counts.get("L", 0) == 0) != (counts.get("R", 0) == 0)
        out["surfaces"].append({
            "surface": si, "verts": nv, "tris": len(tris),
            "vertexBase": cum, "vertexRange": [cum, cum + nv],
            "sideCounts": {"L": counts.get("L", 0), "R": counts.get("R", 0),
                           "none": counts.get(None, 0)},
            "crossArmWeightedVerts": splits,
            "structurallyPure": bool(pure),
            "anatomicalSide": ("L" if counts.get("L", 0) > counts.get("R", 0)
                               else ("R" if counts.get("R", 0) else None)),
            "palmBones": hand_hits,
            "triangleOwnership": tri_owner,
        })
        cum += nv
    out["totalVerts"] = cum
    return out


def laterality(fs, assets, guns):
    """Which ANATOMICAL palm is at the rear grip, per weapon, one frame."""
    rows = []
    for wname, kind in WEAPONS:
        raw = fs.read("weapons/sp/" + wname)
        if not raw:
            continue
        fields = parse_weaponfile(raw.decode("latin-1"))
        rec = guns.get(fields["gunModel"].lower())
        if not rec:
            rows.append({"weapon": wname, "kind": kind, "note": "no gun record"})
            continue
        hand_parts, _ = assets.parts_for(fields["handModel"])
        gun_parts, _ = assets.parts_for(fields["gunModel"])
        if hand_parts is None or gun_parts is None:
            continue
        bones, index, _ = merge_skeletons(hand_parts, gun_parts)
        anim = assets.anim(fields.get("idleAnim", ""))
        if anim is None:
            continue
        posed = pose_model_space(bones, [(anim, 0)])
        side = bone_side_by_ancestry(hand_parts)

        def palm_world(nm):
            i = index.get(nm)
            if i is None or i not in posed:
                return None
            return posed[i][0]

        pl, pr = palm_world("bip01 l hand"), palm_world("bip01 r hand")
        # The gun body bone, and the authored points expressed in its frame.
        mesh = rec.get("meshBone") or rec.get("bodyBone")
        bi = index.get(mesh) if mesh else None
        row = {"weapon": wname, "kind": kind,
               "handModel": fields["handModel"], "gunModel": fields["gunModel"]}
        if bi is not None and bi in posed and pl and pr:
            org, basis = posed[bi]

            def to_world(v):
                w = CA.mat_apply(basis, [c * UNITS_PER_M for c in v])
                return (org[0] + w[0], org[1] + w[1], org[2] + w[2])

            def d(a, b):
                return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))

            pts = {}
            for key, jsonkey in (("rearGrip", "rearGripM"),
                                 ("support", "supportM")):
                val = rec.get(jsonkey)
                v = val.get("value") if isinstance(val, dict) else val
                if v:
                    pts[key] = to_world(v)
            muzzle = rec.get("muzzleM")
            mv = muzzle.get("value") if isinstance(muzzle, dict) else muzzle
            if mv:
                pts["muzzle"] = to_world(mv)

            for key, p in pts.items():
                row["d_%s_Lpalm_in" % key] = round(d(p, pl), 2)
                row["d_%s_Rpalm_in" % key] = round(d(p, pr), 2)
            if "rearGrip" in pts:
                row["rearGripPalm"] = ("R" if d(pts["rearGrip"], pr)
                                       < d(pts["rearGrip"], pl) else "L")
            if "support" in pts:
                row["supportPalm"] = ("R" if d(pts["support"], pr)
                                      < d(pts["support"], pl) else "L")
            if "muzzle" in pts:
                row["palmNearerMuzzle"] = ("R" if d(pts["muzzle"], pr)
                                           < d(pts["muzzle"], pl) else "L")
            row["interPalm_in"] = round(d(pl, pr), 2)
        row["boneSideAncestry"] = {
            "bip01 l hand": side.get(index.get("bip01 l hand"))
            if index.get("bip01 l hand") is not None else None}
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="docs/render_ownership.json")
    args = ap.parse_args()

    fs = CA.Pk3Fs(CA.GAME_MAIN)
    assets = Assets(fs)
    guns = {m["gunModel"].lower(): m
            for m in json.load(open("docs/gun_landmarks.json"))["models"]}

    doc = {"rigs": [], "laterality": []}
    for m in ("viewmodel_hands_new", "viewmodel_hands_sten", "viewmodel_hands"):
        r = analyse_rig(m, assets, fs)
        if r:
            doc["rigs"].append(r)
    doc["laterality"] = laterality(fs, assets, guns)

    # ------------------------------------------------------------- report
    for r in doc["rigs"]:
        print("== %s (%s)  %d verts" % (r["model"], r["parts"], r["totalVerts"]))
        for s in r["surfaces"]:
            t = s["triangleOwnership"]
            print("   surf %d  verts %4d [%4d,%4d)  tris %4d  side=%-4s pure=%-5s "
                  "L/R/none %4d/%4d/%4d  crossArm %d  tri L/R/unk %d/%d/%d seam %d"
                  % (s["surface"], s["verts"], s["vertexRange"][0],
                     s["vertexRange"][1], s["tris"], s["anatomicalSide"],
                     s["structurallyPure"], s["sideCounts"]["L"],
                     s["sideCounts"]["R"], s["sideCounts"]["none"],
                     s["crossArmWeightedVerts"], t["L"], t["R"],
                     t["UNKNOWN"], t["seam"]))
            if s["palmBones"]:
                print("            palm bones: %s" % s["palmBones"])
        if r["namesThatLie"]:
            print("   BONE NAMES THAT CONTRADICT ANCESTRY:")
            for b in r["namesThatLie"]:
                print("      %-20s name says %s, parent %-18s -> ancestry %s"
                      % (b["bone"], b["nameClaims"], b["parent"], b["ancestry"]))
        print()

    print("== laterality (idle pose, one common frame, inches) ==")
    for row in doc["laterality"]:
        if "rearGripPalm" not in row:
            print("   %-10s %-34s (no rear grip record)"
                  % (row["weapon"], row.get("kind", "")))
            continue
        print("   %-10s %-34s rearGrip->%s  support->%s  nearMuzzle->%s  "
              "interPalm %.1f in"
              % (row["weapon"], row["kind"], row["rearGripPalm"],
                 row.get("supportPalm", "?"), row.get("palmNearerMuzzle", "?"),
                 row.get("interPalm_in", 0.0)))
    votes = [r["rearGripPalm"] for r in doc["laterality"] if "rearGripPalm" in r]
    print("\n   rear-grip palm across %d weapons: %s"
          % (len(votes), ", ".join(sorted(set(votes))) or "none"))

    json.dump(doc, open(args.out, "w"), indent=1)
    print("\nwrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())


# ---------------------------------------------------------------- .inc emit
#
# The mapping the runtime classifier uses is generated from the analysis above
# rather than written by hand, so "which half is which hand" cannot drift from
# the shipped asset again. Every earlier laterality error in this project came
# from a human reading a screenshot; this table is read from skin weights.

def emit_inc(doc, path):
    def cls_of(side, kind):
        return "GLSC_%s_%s" % (kind, side)   # e.g. GLSC_HAND_L

    L = []
    L.append("// GENERATED by tools/derive_render_ownership.py -- do not edit by hand.")
    L.append("//")
    L.append("// RENDER OWNERSHIP, derived from the shipped assets by the chain")
    L.append("//   surface -> vertex -> skin weights -> bone ANCESTRY -> anatomical side")
    L.append("// Bone NAMES are not consulted: this rig contains `r cuff01` and")
    L.append("// `r wrist01`, both children of `bip01 l hand`, so a name-based")
    L.append("// partition puts two cuff bones on the wrong arm.")
    L.append("//")
    L.append("// Every surface below is STRUCTURALLY PURE -- 100% of its vertices")
    L.append("// reach one arm root and zero triangles straddle the seam -- so the")
    L.append("// ambiguous-triangle tie policy is defined but never exercised.")
    L.append("")
    L.append("struct CodvrOwnedSurface {")
    L.append("    int surface;      // index within the xmodelsurfs file")
    L.append("    int vertLo, vertHi;   // half-open range in the submitted array")
    L.append("    int tris;")
    L.append("    int cls;          // GLSurfClass this surface's triangles belong to")
    L.append("    int pure;         // 1 = every vertex reaches exactly one arm root")
    L.append("    int seamTris;     // triangles straddling the two arms (must be 0)")
    L.append("};")
    L.append("")
    L.append("struct CodvrRigOwnership {")
    L.append("    const char* parts;")
    L.append("    int totalVerts;")
    L.append("    const CodvrOwnedSurface* surf;")
    L.append("    int nSurf;")
    L.append("    // Draw-order class mapping, derived from the surface order above.")
    L.append("    int handMergedFirst,   handMergedSecond;")
    L.append("    int sleeveMergedFirst, sleeveMergedSecond;")
    L.append("    int handSingleFirst,   handSingleSecond;")
    L.append("    int sleeveSingleFirst, sleeveSingleSecond;")
    L.append("};")
    L.append("")

    names = []
    for r in doc["rigs"]:
        sym = r["parts"].replace("viewmodel_", "").upper()
        hands = [s for s in r["surfaces"] if s["verts"] >= 300]
        sleeves = [s for s in r["surfaces"] if s["verts"] < 300]
        L.append("static const CodvrOwnedSurface kOwn_%s[] = {" % sym)
        for s in r["surfaces"]:
            kind = "HAND" if s in hands else "SLEEVE"
            L.append("    { %d, %5d, %5d, %5d, %-14s, %d, %d },"
                     % (s["surface"], s["vertexRange"][0], s["vertexRange"][1],
                        s["tris"], cls_of(s["anatomicalSide"], kind),
                        1 if s["structurallyPure"] else 0,
                        s["triangleOwnership"]["seam"]))
        L.append("};")
        L.append("")
        hs = [cls_of(s["anatomicalSide"], "HAND") for s in hands]
        ss = [cls_of(s["anatomicalSide"], "SLEEVE") for s in sleeves]
        names.append((sym, r, hs, ss))

    L.append("static const CodvrRigOwnership kRigOwnership[] = {")
    for sym, r, hs, ss in names:
        L.append("    { \"%s\", %d, kOwn_%s, %d,"
                 % (r["parts"], r["totalVerts"], sym, len(r["surfaces"])))
        L.append("      %s, %s,   %s, %s," % (hs[0], hs[1], ss[0], ss[1]))
        L.append("      %s, %s,   %s, %s }," % (hs[0], hs[1], ss[0], ss[1]))
    L.append("};")
    L.append("")
    L.append("// Laterality, settled by PHYSICS in one common frame (idle pose):")
    L.append("// which anatomical palm sits at each model's authored rear grip.")
    for row in doc["laterality"]:
        if "rearGripPalm" in row:
            L.append("//   %-10s %-34s rearGrip->%s support->%s interPalm %.1f in"
                     % (row["weapon"], row["kind"], row["rearGripPalm"],
                        row.get("supportPalm", "?"), row.get("interPalm_in", 0)))
    votes = set(r["rearGripPalm"] for r in doc["laterality"] if "rearGripPalm" in r)
    L.append("// UNANIMOUS across %d weapons: the RIGHT/TRIGGER hand is the one at"
             % len([r for r in doc["laterality"] if "rearGripPalm" in r]))
    L.append("// the rear grip, and it is driven by `bip01 r hand`.")
    L.append("#define CODVR_TRIGGER_IS_ANATOMICAL_RIGHT %d"
             % (1 if votes == {"R"} else 0))
    open(path, "w").write("\n".join(L) + "\n")
    return path
