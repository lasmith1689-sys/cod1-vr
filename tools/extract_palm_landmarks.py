#!/usr/bin/env python
"""Rigid palm landmarks for EVERY shipped first-person hand rig.

THE BLOCKER (and the way through)
---------------------------------
The engine skins hands on the CPU and submits already-posed vertices; the
authored palm transform is recoverable in closed form from vertices bound
RIGIDLY to one palm bone (submitted = palmTransform * bindLocal, exactly,
every frame). See src/vr_handpose.h for the solver contract.

WHY MULTI-RIG (2026-08-27)
--------------------------
The 15:53 attended run solved 687 palms per hand and refused the rest at the
rms gate. Three hand rigs ship; the original table covered only
`viewmodel_hands_new`. `viewmodel_hands_sten` (fg42/mp40/ppsh/sten weapons)
has IDENTICAL surface sizes and index counts -- the runtime cannot tell them
apart by batch fingerprint, so the solver must try both bind clouds and let
the micrometre residual decide. `viewmodel_hands` (790 verts, per-surface
302-vertex hand draws) is shipped but selected by no SP weapon; it is covered
anyway so an unexpected selection degrades to a solved hand instead of a
vanished one.

SURFACE AND SIDE DERIVATION -- STRUCTURAL, NEVER BY CONVENTION
--------------------------------------------------------------
Surface order differs between rigs (tools/selftest_geometry.py asserts it so
nothing can hardcode it). Hand surfaces are found by the flag byte: a surface
whose influences ALL carry BONE_FLAG_HAND is a hand shell; its asset side
comes from the flag byte + parent chain (bone NAMES lie on these rigs:
`r cuff01` is parented to the LEFT hand).

LATERALITY -- READ BEFORE CHANGING (CORRECTED 2026-08-28)
---------------------------------------------------------
The asset side IS the rendered side: `bip01 r hand` is the player's RIGHT
(trigger) hand. The 2026-08-25 sweep concluded the opposite, but it read
SUBMITTED-space coordinates as eye coordinates; the hand batch's modelview
is not identity. Two independent measurements settle it:

  1. The live 15:53 capture's hand modelview has det = +1 (no mirror), and
     mapping the solved palms through it puts surface 1 (`bip01 r`) at
     eye (x +4.0, z -1.6) -- the near, right, REAR/trigger hand -- and
     surface 0 (`bip01 l`) at (x +1.0, z -16.6) -- the far FOREGRIP hand.
     (tools/../scratch mvcheck over runs/vbogame-20260827-155348.)
  2. Label-free authored physics: across six weapons the `bip01 r hand`
     palm is 0.54-1.02 m from the muzzle vs 0.25-0.65 m for `bip01 l` --
     the rear hand is the one farther from the muzzle
     (tools/check_grip_hand.py).

This also makes the profile table coherent: rearGripOffset was measured
from `bip01 r hand`, which is exactly the hand the RIGHT controller drives.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA

UNITS_PER_M = 39.3701
RIGID_W = 0.999

# (xmodel name, indexing) -- indexing says what vertex address the runtime
# batch uses for this rig:
#   "merged" : hand surfaces 0..1 are concatenated into one draw; landmark
#              index = surfaceBase + local (proven live for hands_new).
#   "local"  : each hand surface draws alone with its own array; landmark
#              index = local (the GLS_HAND4_INDICES=1149 fingerprint in
#              vr_glsurf.h assumes per-surface hands4 draws).
HAND_MODELS = (
    ("viewmodel_hands_new",  "merged"),
    ("viewmodel_hands_sten", "merged"),
    ("viewmodel_hands",      "local"),
)

LATERALITY_PROVENANCE = {
    "viewmodel_hands_new":  "proven (live modelview eye-space + muzzle physics, 2026-08-28)",
    "viewmodel_hands_sten": "inferred (identical structure to hands_new)",
    "viewmodel_hands":      "inferred (family convention; no SP weapon uses it)",
}


def rigid_verts(surf, bone_index):
    """[(vertexIndex, bindLocalPos)] for vertices owned outright by one bone."""
    out = []
    for i, v in enumerate(surf.verts):
        total = sum(w for _b, _p, w in v.weights) or 1.0
        for b, p, w in v.weights:
            if b == bone_index and (w / total) >= RIGID_W:
                out.append((i, tuple(p)))
                break
    return out


def condition(points):
    """Singular values of the centred landmark cloud (flip-risk witness)."""
    n = len(points)
    if n < 3:
        return [0.0, 0.0, 0.0]
    cx = sum(p[0] for p in points) / n
    cy = sum(p[1] for p in points) / n
    cz = sum(p[2] for p in points) / n
    c = [[0.0] * 3 for _ in range(3)]
    for p in points:
        d = (p[0] - cx, p[1] - cy, p[2] - cz)
        for a in range(3):
            for b in range(3):
                c[a][b] += d[a] * d[b]
    for a in range(3):
        for b in range(3):
            c[a][b] /= n
    return sorted((abs(e) ** 0.5 for e in sym3_eigenvalues(c)), reverse=True)


def sym3_eigenvalues(m):
    p1 = m[0][1] ** 2 + m[0][2] ** 2 + m[1][2] ** 2
    q = (m[0][0] + m[1][1] + m[2][2]) / 3.0
    if p1 < 1e-18:
        return [m[0][0], m[1][1], m[2][2]]
    p2 = ((m[0][0] - q) ** 2 + (m[1][1] - q) ** 2 + (m[2][2] - q) ** 2 + 2 * p1)
    p = (p2 / 6.0) ** 0.5
    b = [[(m[i][j] - (q if i == j else 0.0)) / p for j in range(3)] for i in range(3)]
    detb = (b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1])
            - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0])
            + b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]))
    r = max(-1.0, min(1.0, detb / 2.0))
    import math
    phi = math.acos(r) / 3.0
    e1 = q + 2 * p * math.cos(phi)
    e3 = q + 2 * p * math.cos(phi + (2 * math.pi / 3))
    e2 = 3 * q - e1 - e3
    return [e1, e2, e3]


def farthest_point_subset(rv, k):
    """A spread subset, chosen deterministically."""
    if len(rv) <= k:
        return rv
    chosen = [rv[0]]
    rest = list(rv[1:])
    while len(chosen) < k and rest:
        def mind(item):
            _i, p = item
            return min((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
                       for _j, q in chosen)
        nxt = max(rest, key=mind)
        rest.remove(nxt)
        chosen.append(nxt)
    return chosen


def hand_surfaces(parts, surfs):
    """[(surfaceIndex, assetSide)] for the two hand shells, structurally.

    A hand surface: every influencing bone carries BONE_FLAG_HAND, and the
    flag-byte sides agree on exactly one side. Sleeves reach forearm bones
    (no HAND bit) and are excluded.
    """
    sides = CA.bone_sides(parts)
    out = []
    for s in surfs.surfaces:
        bones = [b for b in s.bones if 0 <= b < len(parts.bones)]
        if not bones:
            continue
        if any((parts.bones[b].flag & CA.BONE_FLAG_HAND) == 0 for b in bones):
            continue                                   # reaches the arm: sleeve
        ss = {sides[b] for b in bones} - {CA.SIDE_NEUTRAL}
        if ss == {CA.SIDE_LEFT}:
            out.append((s.index, "L"))
        elif ss == {CA.SIDE_RIGHT}:
            out.append((s.index, "R"))
    return out


def palm_bone(parts, surf):
    """The bone owning the most rigid vertices on this surface = the palm."""
    best, best_n = None, 0
    for b in sorted(set(surf.bones)):
        if not (0 <= b < len(parts.bones)):
            continue
        n = len(rigid_verts(surf, b))
        if n > best_n:
            best, best_n = b, n
    return best, best_n


def analyse(fs, model_name, indexing, max_keep):
    xm = CA.parse_xmodel(fs.read("xmodel/" + model_name), model_name)
    base = xm.parts_name
    parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + base), base)
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)

    hs = hand_surfaces(parts, surfs)
    if len(hs) != 2:
        raise SystemExit("%s: expected exactly 2 hand surfaces, found %r"
                         % (model_name, hs))

    offsets, run = [], 0
    for s in surfs.surfaces:
        offsets.append(run)
        run += s.num_verts

    doc = {"model": model_name, "parts": base, "indexing": indexing,
           "lateralityProvenance": LATERALITY_PROVENANCE[model_name],
           "surfaceVerts": [s.num_verts for s in surfs.surfaces],
           "hands": []}

    for surf_i, asset_side in hs:
        surf = surfs.surfaces[surf_i]
        bi, _n = palm_bone(parts, surf)
        if bi is None:
            raise SystemExit("%s surf %d: no rigid palm bone" % (model_name, surf_i))
        rv = rigid_verts(surf, bi)
        sv = condition([p for _i, p in rv])
        keep = farthest_point_subset(rv, max_keep)
        # CORRECTED 2026-08-28: the asset side IS the rendered side (see the
        # module docstring). bip01 r hand = player's RIGHT/trigger.
        rendered = "RIGHT" if asset_side == "R" else "LEFT"
        base_off = offsets[surf_i] if indexing == "merged" else 0
        doc["hands"].append({
            "surface": surf_i,
            "vertexBase": base_off,
            "vertexCount": surf.num_verts,
            "palmBone": parts.bones[bi].name,
            "assetSide": asset_side,
            "rendersAs": rendered,
            "rigidCount": len(rv),
            "singularValuesM": [round(v / UNITS_PER_M, 6) for v in sv],
            "landmarks": [{"index": base_off + i,
                           "localIndex": i,
                           "bindM": [round(c / UNITS_PER_M, 6) for c in p]}
                          for i, p in keep],
        })

    # The batch the solver sees: merged rigs concatenate the two hand
    # surfaces; local rigs draw one surface at a time.
    if indexing == "merged":
        lo = min(s for s, _ in hs)
        doc["batchVerts"] = sum(surfs.surfaces[s].num_verts for s, _ in hs)
        doc["splitVert"] = surfs.surfaces[lo].num_verts
    else:
        doc["batchVerts"] = surfs.surfaces[hs[0][0]].num_verts
        doc["splitVert"] = 0
    return doc


def rig_tag(model_name):
    return {"viewmodel_hands_new": "NEW4",
            "viewmodel_hands_sten": "STEN4",
            "viewmodel_hands": "H4"}[model_name]


def write_header(path, docs):
    L = []
    L.append("// GENERATED by tools/extract_palm_landmarks.py -- do not edit by hand.")
    L.append("//")
    L.append("// Rigid palm landmarks (weight >= %.3f to one palm bone) for EVERY" % RIGID_W)
    L.append("// shipped first-person hand rig. submittedPos = palmTransform * bindLocal")
    L.append("// exactly, every frame, so a Kabsch solve against the submitted array")
    L.append("// recovers the palm's full rigid transform. bindM is METRES; the solver")
    L.append("// scales by its unitsPerMetre argument.")
    L.append("//")
    L.append("// LATERALITY (CORRECTED 2026-08-28): the asset side IS the rendered")
    L.append("// side -- bip01 r hand is the player's RIGHT (trigger) hand. Proven by")
    L.append("// the live hand modelview (det +1, palms map to a right-handed hold in")
    L.append("// eye space) and by muzzle-distance physics across six weapons. The")
    L.append("// 2026-08-25 'inversion' read submitted-space coords as eye coords.")
    L.append("//")
    L.append("// hands_new and hands_sten have IDENTICAL batch shapes; only the bind")
    L.append("// clouds differ. The solver must try both and let the micrometre rms")
    L.append("// decide -- that ambiguity is why kHandRigDefs exists.")
    L.append("")
    new4 = next(d for d in docs if d["model"] == "viewmodel_hands_new")
    L.append("#define CODVR_HAND_BATCH_VERTS %d" % new4["batchVerts"])
    L.append("#define CODVR_HAND_SPLIT_VERT  %d" % new4["splitVert"])
    L.append("")
    for d in docs:
        tag = rig_tag(d["model"])
        for h in d["hands"]:
            hand_tag = "TRIG" if h["rendersAs"] == "RIGHT" else "SUPP"
            arr = ("kPalm%s" % hand_tag) if tag == "NEW4" \
                  else ("kPalm%s_%s" % (tag, hand_tag))
            L.append("// %s (%s) surface %d, %s indexing, palm bone \"%s\" (asset %s),"
                     % (d["model"], d["parts"], h["surface"], d["indexing"],
                        h["palmBone"], h["assetSide"]))
            L.append("// rendered as the player's %s hand (%s)."
                     % (h["rendersAs"], d["lateralityProvenance"]))
            L.append("// %d rigid verts, %d kept, spread %.4f / %.4f / %.4f m."
                     % (h["rigidCount"], len(h["landmarks"]),
                        *h["singularValuesM"]))
            L.append("static const CodvrPalmLandmark %s[] = {" % arr)
            for lm in h["landmarks"]:
                L.append("    { %5d, { %10.6ff, %10.6ff, %10.6ff } },"
                         % (lm["index"], *lm["bindM"]))
            L.append("};")
            L.append("")
    L.append("// One row per rig, in try-order (most likely first). batchVerts is the")
    L.append("// vertex span of the draw the tables index into; splitVert is the merged")
    L.append("// half boundary (0 = per-surface single draws).")
    L.append("static const CodvrHandRigDef kHandRigDefs[] = {")
    for d in docs:
        tag = rig_tag(d["model"])
        trig = next(h for h in d["hands"] if h["rendersAs"] == "RIGHT")
        supp = next(h for h in d["hands"] if h["rendersAs"] == "LEFT")
        trig_arr = "kPalmTRIG" if tag == "NEW4" else "kPalm%s_TRIG" % tag
        supp_arr = "kPalmSUPP" if tag == "NEW4" else "kPalm%s_SUPP" % tag
        L.append('    { "%s", %d, %d,' % (d["parts"], d["batchVerts"], d["splitVert"]))
        L.append("      %s, (int)(sizeof(%s) / sizeof(%s[0]))," % (trig_arr, trig_arr, trig_arr))
        L.append("      %s, (int)(sizeof(%s) / sizeof(%s[0])) }," % (supp_arr, supp_arr, supp_arr))
    L.append("};")
    L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--json", default="docs/palm_landmarks.json")
    ap.add_argument("--header", default="src/vr_palm_landmarks.inc")
    ap.add_argument("--max", type=int, default=48,
                    help="landmarks kept per hand (the solve needs 3)")
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    docs = []
    for name, indexing in HAND_MODELS:
        docs.append(analyse(fs, name, indexing, args.max))

    # Structural assertion backing the sten laterality inference: sten must
    # mirror hands_new surface-for-surface or the inference is void.
    new4 = next(d for d in docs if d["model"] == "viewmodel_hands_new")
    sten = next(d for d in docs if d["model"] == "viewmodel_hands_sten")
    if (new4["surfaceVerts"] != sten["surfaceVerts"]
            or [(h["surface"], h["assetSide"]) for h in new4["hands"]]
            != [(h["surface"], h["assetSide"]) for h in sten["hands"]]):
        raise SystemExit("sten4 is no longer structurally identical to new4; "
                         "its inferred laterality must be re-established")

    with open(args.json, "w") as f:
        json.dump({"rigs": docs}, f, indent=1)
    write_header(args.header, docs)

    for d in docs:
        print("%s (%s, %s indexing, batch %d/split %d)  [%s]"
              % (d["model"], d["parts"], d["indexing"], d["batchVerts"],
                 d["splitVert"], d["lateralityProvenance"]))
        for h in d["hands"]:
            print("  surf %d  base %4d  verts %4d  palm %-16s asset %s -> renders %-5s"
                  " rigid %d kept %d  spread %.4f/%.4f/%.4f m"
                  % (h["surface"], h["vertexBase"], h["vertexCount"],
                     '"%s"' % h["palmBone"], h["assetSide"], h["rendersAs"],
                     h["rigidCount"], len(h["landmarks"]),
                     *h["singularValuesM"]))
    print()
    print("wrote %s and %s" % (args.json, args.header))
    return 0


if __name__ == "__main__":
    sys.exit(main())
