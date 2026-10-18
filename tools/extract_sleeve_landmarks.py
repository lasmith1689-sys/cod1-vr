#!/usr/bin/env python
"""Forearm landmarks on the SLEEVE surfaces, and the anatomical palm->forearm
rest relation, per shipped hand rig and side (brief 2026-09-04, P1).

THE SLEEVE IS NOT RIGID. Measured here: the hands_new4 sleeve surface (120
verts) is skinned across forearm, upper arm, cuff, wrist and hand bones,
with 8 (right) / 0 (left) vertices fully rigid to the forearm. So the
sleeve cannot be solved the way the palm shell is. What CAN be recovered
is the forearm's orientation, from the vertices the forearm dominates
(weight >= FOREARM_W): with a wider residual gate the Kabsch solve lands
within ~3 deg / 1 cm of the true forearm (scratch measurement over the
BAR/Bren idle and fire animations, 2026-09-04). The runtime uses that
orientation only; the wrist POINT comes from the palm solve, exactly.

THE REST RELATION. The bind pose stores no translations, and the animated
palm->forearm relation varies 4..76 deg of wrist bend across weapons, so
a fixed authored relation is CONSTRUCTED: the forearm's long axis (elbow
-> wrist, the forearm bone's +X, wrist at +0.294 m) continues the hand's
knuckle direction k (a straight wrist); its roll about that axis is the
mean of the idle animations, projected. Forearm length is the constant
bone length. Output: docs/sleeve_landmarks.json, src/vr_sleeve_landmarks.inc.
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space, basis_to_local
from extract_gun_landmarks import parse_weaponfile, farthest_subset, spread, UNITS_PER_M
from extract_palm_landmarks import hand_surfaces, palm_bone
from derive_palm_calibration import v_norm, v_sub, v_dot, v_cross, v_scale, to_local

FOREARM_W = 0.75
RMS_GATE_M = 0.025     # measured 6-16 mm on the shipped rigs


def sleeve_surfaces(parts, surfs):
    """[(surfaceIndex, side, forearmBoneIndex)] -- surfaces reaching a forearm bone."""
    hs = {s for s, _ in hand_surfaces(parts, surfs)}
    out = []
    for s in surfs.surfaces:
        if s.index in hs:
            continue
        for b in sorted(set(s.bones)):
            nm = parts.bones[b].name.lower()
            if nm.endswith(" forearm"):
                side = "L" if " l " in nm else "R"
                out.append((s.index, side, b))
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--json", default="docs/sleeve_landmarks.json")
    ap.add_argument("--header", default="src/vr_sleeve_landmarks.inc")
    ap.add_argument("--max", type=int, default=24)
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)

    # Which weapons use which hand rig (for posing).
    users = {}
    for path in fs.list("weapons/sp/"):
        f = parse_weaponfile(fs.read(path).decode("latin-1"))
        hm = f.get("handModel", "")
        if hm:
            users.setdefault(hm.lower(), []).append((os.path.basename(path), f))

    docs = []
    for model_name in ("viewmodel_hands_new", "viewmodel_hands_sten", "viewmodel_hands"):
        raw = fs.read("xmodel/" + model_name)
        if not raw:
            continue
        xm = CA.parse_xmodel(raw, model_name)
        base = xm.parts_name
        parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + base), base)
        surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
        weapons = users.get(model_name.lower(), [])
        hs = dict(hand_surfaces(parts, surfs))          # surf -> side
        ss = sleeve_surfaces(parts, surfs)
        doc = {"model": model_name, "parts": base, "weapons": [w for w, _ in weapons][:6],
               "sides": []}
        for surf_i, side, fb in ss:
            surf = surfs.surfaces[surf_i]
            # Forearm-dominated vertices, positions in the forearm frame.
            lm = []
            for i, v in enumerate(surf.verts):
                tot = sum(w for _b, _p, w in v.weights) or 1.0
                for b, p, w in v.weights:
                    if b == fb and (w / tot) >= FOREARM_W:
                        lm.append((i, tuple(p)))
            rigid = sum(1 for i, v in enumerate(surf.verts)
                        for b, p, w in v.weights if b == fb and w >= 0.999)
            keep = farthest_subset(lm, args.max)
            rec = {"surface": surf_i, "side": side, "rendersAs": "RIGHT" if side == "R" else "LEFT",
                   "vertexCount": surf.num_verts, "forearmBone": parts.bones[fb].name,
                   "dominatedVerts": len(lm), "rigidVerts": rigid,
                   "landmarks": [{"index": i, "bindM": [round(c / UNITS_PER_M, 6) for c in p]}
                                 for i, p in keep],
                   "minSpreadM": round(spread([p for _i, p in keep]) / UNITS_PER_M, 6)}
            # The palm surface of this side, for the knuckle direction.
            palm_surf = [s for s, sd in hs.items() if sd == side]
            hand_bi = palm_bone(parts, surfs.surfaces[palm_surf[0]])[0] if palm_surf else None
            rest = None
            if weapons and hand_bi is not None:
                hand_name = parts.bones[hand_bi].name
                ks, ys, lens, bends = [], [], [], []
                for wname, f in weapons[:8]:
                    gun_parts, _ = assets.parts_for(f.get("gunModel", ""))
                    anim = assets.anim(f.get("idleAnim", ""))
                    if gun_parts is None or anim is None:
                        continue
                    bones, index, _ = merge_skeletons(parts, gun_parts)
                    hi = index.get(hand_name.lower())
                    fi = index.get(parts.bones[fb].name.lower())
                    if hi is None or fi is None:
                        continue
                    n = anim.num_frames
                    for fr in range(0, n, max(1, n // 4)):
                        posed = pose_model_space(bones, [(anim, fr)])
                        h_org, h_basis = posed[hi]
                        f_org, f_basis = posed[fi]
                        prefix = "bip01 %s finger" % side.lower()
                        roots = []
                        for nm, bidx in index.items():
                            if nm.startswith(prefix) and len(nm) == len(prefix) + 1 and nm[-1] in "1234":
                                roots.append(to_local(h_basis, h_org, posed[bidx][0]))
                        if len(roots) != 4:
                            continue
                        k = v_norm(tuple(sum(r[i] for r in roots) / 4.0 for i in range(3)))
                        ks.append(k)
                        # forearm +Y in the palm frame (roll reference)
                        ys.append(basis_to_local(h_basis, list(f_basis[3:6])))
                        d = [h_org[c] - f_org[c] for c in range(3)]
                        lens.append(math.sqrt(sum(c * c for c in d)) / UNITS_PER_M)
                        e = v_norm(tuple(-c for c in to_local(h_basis, h_org, f_org)))
                        bends.append(math.degrees(math.acos(max(-1.0, min(1.0, v_dot(e, k))))))
                if ks:
                    k = v_norm(tuple(sum(v[i] for v in ks) / len(ks) for i in range(3)))
                    ym = tuple(sum(v[i] for v in ys) / len(ys) for i in range(3))
                    y = v_norm(v_sub(ym, v_scale(k, v_dot(ym, k))))
                    z = v_cross(k, y)
                    rest = {"rows": [list(k), list(y), list(z)],
                            "forearmLenM": round(sum(lens) / len(lens), 6),
                            "lenSpreadM": round(max(lens) - min(lens), 6),
                            "animBendDegMin": round(min(bends), 1),
                            "animBendDegMean": round(sum(bends) / len(bends), 1),
                            "animBendDegMax": round(max(bends), 1),
                            "frames": len(ks)}
            rec["rest"] = rest
            doc["sides"].append(rec)
        docs.append(doc)

    with open(args.json, "w") as f:
        json.dump({"rigs": docs}, f, indent=1)
    write_header(args.header, docs)
    for d in docs:
        for s in d["sides"]:
            r = s["rest"] or {}
            print("%-22s surf %d %s (%s): %d dominated (%d rigid), %d kept, spread %.4f m; "
                  "len %.4f m, anim bend %.1f..%.1f deg over %d frames"
                  % (d["parts"], s["surface"], s["rendersAs"], s["forearmBone"],
                     s["dominatedVerts"], s["rigidVerts"], len(s["landmarks"]), s["minSpreadM"],
                     r.get("forearmLenM", 0.0), r.get("animBendDegMin", 0.0),
                     r.get("animBendDegMax", 0.0), r.get("frames", 0)))
    return 0


def write_header(path, docs):
    L = []
    L.append("// GENERATED by tools/extract_sleeve_landmarks.py -- do not edit by hand.")
    L.append("//")
    L.append("// Forearm landmarks on each rig's SLEEVE surfaces (vertices the forearm")
    L.append("// bone dominates, weight >= %.2f), positions in the FOREARM bone frame," % FOREARM_W)
    L.append("// LOCAL indices within the sleeve surface; and the constructed palm->")
    L.append("// forearm REST relation: restRows = forearm axes (X elbow->wrist, Y, Z)")
    L.append("// in the palm-bone frame with the wrist straight (X = knuckle direction),")
    L.append("// roll from the idle animations. The sleeve is skinned, so the solve is")
    L.append("// an orientation estimate (~3 deg) behind a %.0f mm gate; the wrist point" % (RMS_GATE_M * 1000))
    L.append("// itself comes from the palm solve.")
    L.append("")
    for d in docs:
        tag = {"viewmodel_hands_new": "NEW4", "viewmodel_hands_sten": "STEN4",
               "viewmodel_hands": "H4"}.get(d["model"], d["model"].upper())
        for s in d["sides"]:
            if not s["landmarks"] or not s["rest"]:
                continue
            L.append("// %s surface %d, forearm bone \"%s\", rendered %s: %d dominated verts, %d kept."
                     % (d["parts"], s["surface"], s["forearmBone"], s["rendersAs"],
                        s["dominatedVerts"], len(s["landmarks"])))
            L.append("static const CodvrPalmLandmark kSleeve%s_%s[] = {" % (tag, s["side"]))
            for lm in s["landmarks"]:
                L.append("    { %5d, { %10.6ff, %10.6ff, %10.6ff } }," % (lm["index"], *lm["bindM"]))
            L.append("};")
    L.append("")
    L.append("static const CodvrSleeveDef kSleeveDefs[] = {")
    for d in docs:
        tag = {"viewmodel_hands_new": "NEW4", "viewmodel_hands_sten": "STEN4",
               "viewmodel_hands": "H4"}.get(d["model"], d["model"].upper())
        for s in d["sides"]:
            if not s["landmarks"] or not s["rest"]:
                continue
            r = s["rest"]
            rows = r["rows"]
            hand = "VRHP_TRIGGER" if s["side"] == "R" else "VRHP_SUPPORT"
            L.append("    // %s %s: anim wrist bend %.1f..%.1f deg (mean %.1f); rest is straight"
                     % (d["parts"], s["rendersAs"], r["animBendDegMin"], r["animBendDegMax"],
                        r["animBendDegMean"]))
            L.append("    { \"%s\", %s, %d, kSleeve%s_%s, %d," % (d["parts"], hand, s["vertexCount"],
                                                                tag, s["side"], len(s["landmarks"])))
            L.append("      { %9.6ff, %9.6ff, %9.6ff,  %9.6ff, %9.6ff, %9.6ff,  %9.6ff, %9.6ff, %9.6ff },"
                     % tuple(rows[0] + rows[1] + rows[2]))
            L.append("      %9.6ff, %9.6ff }," % (r["forearmLenM"], RMS_GATE_M))
    L.append("};")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


if __name__ == "__main__":
    sys.exit(main())
