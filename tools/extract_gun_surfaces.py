#!/usr/bin/env python
"""Per-SURFACE gun landmarks, every surface of every shipped gun model.

WHY (hardware 2026-09-04c, logs/hw-2026-09-04c): the engine draws a
weapon's surfaces in SHADER-SORTED order, not asset order, so for the
Colt and the Thompson a non-body surface reaches OpenGL before the body
surface that keys the scene's one rigid delta. Those pre-key batches were
drawn head-coupled ("vanilla") while the body went to the controller: two
weapons on screen. The BAR never showed it only because its body happens
to sort first (0 vanilla batches in 12,000 scenes; Colt 1.24 and Thompson
1.10 vanilla batches per scene, zero solve refusals).

Every gun surface the game ships is bound 100 % rigidly to ONE bone
(measured here, asserted below). So a landmark table per surface, with
positions expressed in the BODY bone's frame, lets ANY body-bound surface
key the body pose exactly, whichever the engine draws first. Surfaces
bound to a moving part (bolt, clip, trigger, sight) are emitted too, for
identification, but flagged bodyRigid=0: a rigid part that has moved with
its animation still fits its own table perfectly, so its solve would
report the PART's pose as the body's -- it must never key the scene.

Output: docs/gun_surfaces.json and src/vr_gun_surfaces.inc.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import (Assets, merge_skeletons, pose_model_space,
                              basis_to_local, primary_mesh_bone)
from extract_gun_landmarks import (parse_weaponfile, farthest_subset, spread,
                                   UNITS_PER_M)

# A body-bound surface may key the scene only when its landmark cloud has
# enough extent to fix the body's ORIENTATION: 8+ points and at least 2 cm
# of spread on the thinnest axis. (A 10-vertex sliver would solve, with an
# orientation no better than the noise.)
KEY_MIN_POINTS = 8
KEY_MIN_SPREAD_M = 0.02
REST_TOL_M = 0.001    # part-to-body drift over the idle animation


def surface_bone(surf):
    """The single bone every vertex of `surf` is bound to, or None."""
    bones = set()
    for v in surf.verts:
        total = sum(w for _b, _p, w in v.weights) or 1.0
        for b, _p, w in v.weights:
            if (w / total) > 1e-6:
                bones.add(b)
    return bones.pop() if len(bones) == 1 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--json", default="docs/gun_surfaces.json")
    ap.add_argument("--header", default="src/vr_gun_surfaces.inc")
    ap.add_argument("--max", type=int, default=24)
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)
    models = {}
    for path in fs.list("weapons/sp/"):
        name = os.path.basename(path)
        fields = parse_weaponfile(fs.read(path).decode("latin-1"))
        gm = fields.get("gunModel", "")
        if not gm or gm.lower() in models:
            continue
        model = assets.model(gm)
        if model is None or not model.parts_name:
            continue
        base = model.parts_name
        try:
            surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
            gun_parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + base), base)
        except Exception:
            continue
        body = primary_mesh_bone(gun_parts)
        if body is None:
            continue
        names = {b.index: b.name for b in gun_parts.bones}

        # Posed frames of the merged rig over the idle animation, to express
        # part-bone vertices in the body frame at REST and to measure how
        # still that rest is. Falls back to the bind pose.
        hand_parts, _ = assets.parts_for(fields.get("handModel", ""))
        posed_frames = []
        index = {}
        if hand_parts is not None:
            bones, index, _anchor = merge_skeletons(hand_parts, gun_parts)
            idle = assets.anim(fields.get("idleAnim", ""))
            if idle is not None:
                n = idle.num_frames
                for fr in range(0, n, max(1, n // 8)):
                    posed_frames.append(pose_model_space(bones, [(idle, fr)]))
            if not posed_frames:
                posed_frames.append(pose_model_space(bones, []))
        bi = index.get(body.name.lower())

        rec = {"gunModel": gm, "bodyKey": 0, "bodyBone": body.name,
               "surfaces": []}
        for surf in surfs.surfaces:
            b = surface_bone(surf)
            srec = {"surface": surf.index, "vertexCount": surf.num_verts,
                    "bone": names.get(b, "?") if b is not None else "MIXED",
                    "bodyRigid": 1 if b == body.index else 0,
                    "restStable": 0, "restDriftM": None, "landmarks": [],
                    "minSpreadM": 0.0, "keyUsable": 0}
            if b is None:
                rec["surfaces"].append(srec)
                continue
            # Vertex positions in the BODY frame.
            if b == body.index:
                pts = [(i, tuple(v.weights[0][1])) for i, v in enumerate(surf.verts)]
                srec["restStable"] = 1
                srec["restDriftM"] = 0.0
            else:
                pi = index.get(names.get(b, "").lower())
                if pi is None or bi is None or not posed_frames \
                        or any(pi not in p or bi not in p for p in posed_frames):
                    rec["surfaces"].append(srec)
                    continue
                per_frame = []
                for posed in posed_frames:
                    po, pb = posed[pi]
                    bo, bb = posed[bi]
                    pts_f = []
                    for i, v in enumerate(surf.verts):
                        p = v.weights[0][1]
                        world = [po[k] + CA.mat_apply(pb, p)[k] for k in range(3)]
                        pts_f.append(basis_to_local(bb, [world[k] - bo[k]
                                                         for k in range(3)]))
                    per_frame.append(pts_f)
                n = len(surf.verts)
                mean = [tuple(sum(fr[i][k] for fr in per_frame) / len(per_frame)
                              for k in range(3)) for i in range(n)]
                drift = max(max(abs(fr[i][k] - mean[i][k]) for k in range(3))
                            for fr in per_frame for i in range(n)) if n else 0.0
                srec["restDriftM"] = round(drift / UNITS_PER_M, 6)
                srec["restStable"] = 1 if drift / UNITS_PER_M <= REST_TOL_M else 0
                pts = [(i, mean[i]) for i in range(n)]
            keep = farthest_subset(pts, args.max)
            srec["landmarks"] = [{"index": i,
                                  "bindM": [round(c / UNITS_PER_M, 6) for c in p]}
                                 for i, p in keep]
            srec["minSpreadM"] = round(spread([p for _i, p in keep]) / UNITS_PER_M, 6)
            srec["keyUsable"] = 1 if (srec["bodyRigid"]
                                     and len(keep) >= KEY_MIN_POINTS
                                     and srec["minSpreadM"] >= KEY_MIN_SPREAD_M) else 0
            rec["surfaces"].append(srec)

        # The model key is the body surface's count, exactly as
        # vr_gun_landmarks.inc keys kGunModels (largest body-bound surface).
        body_surfs = [s for s in rec["surfaces"] if s["bodyRigid"]]
        if body_surfs:
            rec["bodyKey"] = max(body_surfs, key=lambda s: s["vertexCount"])["vertexCount"]
        models[gm.lower()] = rec

    out = {"models": sorted(models.values(), key=lambda r: r["gunModel"].lower())}
    with open(args.json, "w") as f:
        json.dump(out, f, indent=1)
    write_header(args.header, out)

    print("%-26s %-5s %s" % ("model", "key", "surfaces  (idx:verts bone flags)"))
    print("-" * 100)
    for m in out["models"]:
        cells = []
        for s in m["surfaces"]:
            fl = ("B" if s["bodyRigid"] else "p") + ("K" if s["keyUsable"] else "-") \
                 + ("r" if s["restStable"] else "!")
            cells.append("%d:%d %s %s" % (s["surface"], s["vertexCount"], s["bone"], fl))
        print("%-26s %-5d %s" % (m["gunModel"], m["bodyKey"], " | ".join(cells)))
    return 0


def write_header(path, out):
    L = []
    L.append("// GENERATED by tools/extract_gun_surfaces.py -- do not edit by hand.")
    L.append("//")
    L.append("// One entry per gun SURFACE (every surface of every shipped gun model),")
    L.append("// landmarks LOCAL to that surface, positions in the model's BODY-bone")
    L.append("// frame at the idle rest pose. bodyRigid=1: the surface is bound to the")
    L.append("// body bone, so a solve against it IS the body pose (keyUsable adds the")
    L.append("// point-count / spread requirement for a trustworthy orientation).")
    L.append("// bodyRigid=0: a moving part -- identification only, NEVER a key: a")
    L.append("// part that has moved with its animation still fits its own table and")
    L.append("// would report its own pose as the body's.")
    L.append("")
    for m in out["models"]:
        tag = "".join(c if c.isalnum() else "_" for c in m["gunModel"])
        for s in m["surfaces"]:
            if len(s["landmarks"]) < 3:
                continue
            L.append("static const CodvrPalmLandmark kGunSurf_%s_s%d[] = {" % (tag, s["surface"]))
            for lm in s["landmarks"]:
                L.append("    { %5d, { %10.6ff, %10.6ff, %10.6ff } },"
                         % (lm["index"], *lm["bindM"]))
            L.append("};")
    L.append("")
    L.append("static const CodvrGunSurface kGunSurfaces[] = {")
    for m in out["models"]:
        tag = "".join(c if c.isalnum() else "_" for c in m["gunModel"])
        for s in m["surfaces"]:
            if len(s["landmarks"]) < 3:
                continue
            L.append("    { %6d, %d, %6d, kGunSurf_%s_s%d, %d, %d, %d, %d, %9.6ff, \"%s\", \"%s\" },"
                     % (m["bodyKey"], s["surface"], s["vertexCount"], tag, s["surface"],
                        len(s["landmarks"]), s["bodyRigid"], s["keyUsable"],
                        s["restStable"], s["minSpreadM"], m["gunModel"], s["bone"]))
    L.append("};")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


if __name__ == "__main__":
    sys.exit(main())
