#!/usr/bin/env python
"""Gun body landmarks and the rear-grip pivot, per gun MODEL.

WHY THE MODEL AND NOT THE WEAPON
--------------------------------
Eight of the twenty-eight carried weapons share a gun model with another
entry: 30cal/bar/bar_slow all draw `viewmodel_BAR`, thompson and
thompson_semi both draw `viewmodel_thompson`, and so on. Rear grip, barrel and
foregrip are properties of the MESH, so the model is the right key -- and it
happens to be the one the runtime can actually determine.

HOW THE RUNTIME KNOWS WHICH MODEL IT IS DRAWING
-----------------------------------------------
By counting the vertices in the batch. Across all 24 gun models the game ships,
every model has a distinct total vertex count -- checked here, zero collisions.
That makes the count an exact model key, available at draw time, with no
guessing and no reading of protected memory.

This replaces the baked-offset signature the profile layer used to identify
weapons, which was explicitly a HYPOTHESIS about what the measured viewmodel
offset means. A vertex count is not a hypothesis.

THE PIVOT PROBLEM THIS SOLVES
-----------------------------
The first gun_yaw probe rotated the weapon about the camera, swinging it round
the player's head. A pivot has to be a point ON the weapon, and the weapon's
vertices arrive already posed -- so the pivot must be recovered from them, the
same way the palm is (see vr_handpose.h). Landmarks rigidly bound to the gun's
body bone give its transform in view space; the rear grip, expressed in that
bone's frame, then lands where it belongs.

CONFIDENCE
----------
The rear grip itself comes from the ported weapon-profile table, which carries
its own PROVEN/STRONG/WEAK metadata. This tool converts it into the body-bone
frame and re-checks that the conversion is frame-independent -- if the body
bone moves relative to tag_weapon during animation, one fixed offset cannot be
right, and the entry is downgraded rather than shipped as a constant.
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import (Assets, merge_skeletons, pose_model_space,
                              basis_to_local, primary_mesh_bone)

UNITS_PER_M = 39.3701
RIGID_W = 0.999
UNKNOWN, NA, WEAK, STRONG, PROVEN = "UNKNOWN", "NA", "WEAK", "STRONG", "PROVEN"

# How far the body bone may drift relative to tag_weapon across an animation
# before a single fixed offset stops being a description of the weapon.
RIGIDITY_TOL_M = 0.002
# How far the sight line and the muzzle tag's own rotation may disagree before
# the sight line stops being trusted as the bore.
BORE_AGREE_DEG = 5.0


def parse_weaponfile(text):
    parts = text.split(chr(92))
    return {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}


def rigid_verts_one_surface(surf, bone_index):
    """[(LOCAL index, bindLocalPos)] rigid to one bone, within ONE surface."""
    out = []
    for i, v in enumerate(surf.verts):
        total = sum(w for _b, _p, w in v.weights) or 1.0
        for b, p, w in v.weights:
            if b == bone_index and (w / total) >= RIGID_W:
                out.append((i, tuple(p)))
                break
    return out


def body_surface(surfs, bone_index):
    """(surfaceIndex, rigidVerts) for the surface holding the most body-rigid
    vertices.

    THE INDEXING FACT THIS ENCODES (measured 2026-08-27, run
    runs/vbogame-20260827-155348): the engine draws each gun SURFACE as its
    own batch with its own vertex array -- minIdx 0, span == that surface's
    vertex count (the five captured spans sum exactly to the model total).
    The original tables used model-global indices keyed by the model total,
    which no live batch ever presents; they could never have matched a real
    draw. Landmarks are therefore LOCAL to one chosen surface and the model
    key is THAT SURFACE's vertex count.
    """
    best, best_rv = -1, []
    for surf in surfs.surfaces:
        rv = rigid_verts_one_surface(surf, bone_index)
        if len(rv) > len(best_rv):
            best, best_rv = surf.index, rv
    return best, best_rv


def farthest_subset(rv, k):
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


def spread(points):
    if len(points) < 3:
        return 0.0
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    return min(hi[i] - lo[i] for i in range(3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--profiles", default="docs/weapon_geometry.json")
    ap.add_argument("--json", default="docs/gun_landmarks.json")
    ap.add_argument("--header", default="src/vr_gun_landmarks.inc")
    ap.add_argument("--max", type=int, default=24)
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)

    geo = {}
    if os.path.exists(args.profiles):
        raw = json.load(open(args.profiles))
        rows = raw.get("weapons", raw) if isinstance(raw, dict) else raw
        for r in rows:
            geo[r.get("weapon", "")] = r

    models = {}     # gunModel -> record
    counts = {}     # vertexCount -> gunModel, for the collision proof

    for path in fs.list("weapons/sp/"):
        name = os.path.basename(path)
        fields = parse_weaponfile(fs.read(path).decode("latin-1"))
        gm = fields.get("gunModel", "")
        if not gm:
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
        total = sum(s.num_verts for s in surfs.surfaces)

        if gm.lower() in models:
            models[gm.lower()]["weapons"].append(name)
            continue

        body = primary_mesh_bone(gun_parts)
        rec = {
            "boreForward": {"value": None, "confidence": UNKNOWN, "evidence": "not resolved"},
            "muzzleM": {"value": None, "confidence": UNKNOWN, "evidence": "not resolved"},
            "gunModel": gm,
            "parts": base,
            "modelVerts": total,
            "surfaces": [s.num_verts for s in surfs.surfaces],
            "weapons": [name],
            "bodyBone": body.name if body else None,
            "landmarks": [],
            "rearGripM": {"value": None, "confidence": UNKNOWN,
                          "evidence": "not resolved"},
            "supportM": {"value": None, "confidence": UNKNOWN,
                         "evidence": "not resolved"},
            "barrelForward": {"value": None, "confidence": UNKNOWN,
                              "evidence": "not resolved"},
            "supportRadiusM": {"value": None, "confidence": UNKNOWN},
        }

        if body is not None:
            surf_i, rv = body_surface(surfs, body.index)
            keep = farthest_subset(rv, args.max)
            rec["keySurface"] = surf_i
            rec["vertexCount"] = (surfs.surfaces[surf_i].num_verts
                                  if surf_i >= 0 else 0)
            rec["rigidCount"] = len(rv)
            rec["minSpreadM"] = round(spread([p for _i, p in keep]) / UNITS_PER_M, 6)
            rec["landmarks"] = [
                {"index": i, "bindM": [round(c / UNITS_PER_M, 6) for c in p]}
                for i, p in keep]
            # Per-surface counts CAN collide across models (the model totals
            # never did). A collision is survivable -- the runtime tries every
            # matching entry and the micrometre residual picks the real one --
            # but it must be visible, not silent.
            key = rec["vertexCount"]
            if key in counts and counts[key] != gm.lower():
                print("NOTE: key %d shared by %s and %s (runtime disambiguates "
                      "by residual)" % (key, counts[key], gm))
            counts[key] = gm.lower()

        # ---- tag_weapon-frame profile geometry -> the body-bone frame -----
        #
        # UNITS. weapon_geometry.json stores POINTS in INCHES -- its own
        # evidence strings say so ("spread 0.000 in") and
        # gen_weapon_profiles.py divides by 39.37 to reach the table's
        # metres. An earlier pass here read them as metres and multiplied,
        # producing grips two to sixteen METRES from the mesh root; what
        # caught it was noticing that no rifle is 5.6 m long. Directions
        # (barrelForward) are unit vectors: rotation only, no units.
        src = geo.get(name)
        hand_parts, _ = assets.parts_for(fields.get("handModel", ""))
        if src and body is not None and hand_parts is not None:
            bones, index, anchor = merge_skeletons(hand_parts, gun_parts)
            bi = index.get(body.name.lower())
            ti = index.get("tag_weapon")
            idle = assets.anim(fields.get("idleAnim", ""))
            fire = assets.anim(fields.get("fireAnim", ""))
            posed_frames = []
            for anim in (idle, fire):
                if anim is None:
                    continue
                n = anim.num_frames
                for fr in range(0, n, max(1, n // 8)):
                    posed = pose_model_space(bones, [(anim, fr)])
                    if bi in posed and ti in posed:
                        posed_frames.append(posed)

            def to_body(field, is_dir):
                # `field` is a key of the profile, or ("__bore__", vector) for
                # a value this tool derived rather than read.
                if isinstance(field, tuple):
                    ent = {"value": list(field[1]), "confidence": STRONG}
                    field = field[0]
                else:
                    ent = src.get(field, {})
                if not ent.get("value") or not posed_frames:
                    return None
                conf_in = ent.get("confidence", UNKNOWN)
                samples = []
                for posed in posed_frames:
                    to, tb = posed[ti]
                    bo, bb = posed[bi]
                    v = list(ent["value"])
                    if is_dir:
                        world = CA.mat_apply(tb, v)
                        local = basis_to_local(bb, list(world))
                        n = (sum(c * c for c in local)) ** 0.5 or 1.0
                        samples.append([c / n for c in local])
                    else:
                        world = [to[k] + CA.mat_apply(tb, v)[k] for k in range(3)]
                        local = basis_to_local(bb, [world[k] - bo[k]
                                                    for k in range(3)])
                        samples.append([c / UNITS_PER_M for c in local])
                mean = [sum(s[k] for s in samples) / len(samples)
                        for k in range(3)]
                drift = max(max(abs(s[k] - mean[k]) for k in range(3))
                            for s in samples)
                tol = 0.02 if is_dir else RIGIDITY_TOL_M   # dir: ~1.1 deg
                if drift <= tol:
                    conf = conf_in
                    ev = ("profile %s (%s) converted into the %s frame; "
                          "drift %.4f over %d posed frames"
                          % (field, conf_in, body.name, drift,
                             len(samples)))
                else:
                    conf = WEAK
                    ev = ("profile %s (%s) but it drifts %.4f relative to "
                          "the body bone -- one fixed value cannot describe "
                          "it" % (field, conf_in, drift))
                if is_dir:
                    n = (sum(c * c for c in mean)) ** 0.5 or 1.0
                    mean = [c / n for c in mean]
                return {"value": [round(c, 6) for c in mean],
                        "confidence": conf, "evidence": ev,
                        "driftM": round(drift, 6)}

            # tag_weapon's own frame expressed in the body frame -- the bridge
            # that lets tag-frame measurements (the reload zones) be shipped
            # in the frame the runtime actually solves. Same rigidity check.
            if posed_frames:
                psamples = []
                asamples = []
                for posed in posed_frames:
                    to, tb = posed[ti]
                    bo, bb = posed[bi]
                    psamples.append([c / UNITS_PER_M for c in
                                     basis_to_local(bb, [to[k] - bo[k]
                                                         for k in range(3)])])
                    rows = []
                    for r3 in range(3):
                        rows.extend(basis_to_local(bb, list(tb[r3*3:r3*3+3])))
                    asamples.append(rows)
                pmean = [sum(s[k] for s in psamples) / len(psamples)
                         for k in range(3)]
                pdrift = max(max(abs(s[k] - pmean[k]) for k in range(3))
                             for s in psamples)
                amean = [sum(s[k] for s in asamples) / len(asamples)
                         for k in range(9)]
                adrift = max(max(abs(s[k] - amean[k]) for k in range(9))
                             for s in asamples)
                rec["tagInBody"] = {
                    "posM": [round(c, 6) for c in pmean],
                    "axis": [round(c, 6) for c in amean],
                    "posDriftM": round(pdrift, 6),
                    "axisDrift": round(adrift, 6),
                    "rigid": 1 if (pdrift <= RIGIDITY_TOL_M
                                   and adrift <= 0.02) else 0,
                }

            # ---- THE BORE (brief 2026-09-04d, P1, sections 6-8) ----------
            #
            # The axis the shot must follow is the one the user aligns: the
            # weapon's SIGHT LINE, cross-checked against the muzzle tag's own
            # rotation (the flash direction, i.e. the bore by construction).
            # It is NOT the chord from the weapon frame's origin to the muzzle
            # tag, which is what `barrelForward` ships: on a rig whose frame
            # origin is off the bore that chord tilts, and on the Thompson it
            # tilts 6.64 deg ABOVE the sight line -- 0.91 m (three feet) high
            # at 7.9 m, which is the 2026-09-04d hardware report exactly.
            #
            # Preference order, with the disagreement recorded either way:
            #   1. sightForward, when STRONG+ and within BORE_AGREE_DEG of the
            #      muzzle tag's rotation (the two independent measurements)
            #   2. the muzzle tag's own rotation
            #   3. the body mesh's long axis
            #   4. the frame-origin-to-muzzle chord (the old value)
            bf = src.get("barrelForward", {}) or {}
            ests = bf.get("estimates") or {}
            sight = src.get("sightForward", {}) or {}
            def as_field(vec, conf, why):
                return {"value": list(vec), "confidence": conf, "evidence": why}
            cand = []
            if sight.get("value") and sight.get("confidence") in (STRONG, PROVEN):
                cand.append(("sightLine", sight["value"], sight["confidence"]))
            if ests.get("fromRotation"):
                cand.append(("muzzleTagRotation", ests["fromRotation"], bf.get("confidence", WEAK)))
            if ests.get("fromBodyAabb"):
                cand.append(("bodyLongAxis", ests["fromBodyAabb"], WEAK))
            if bf.get("value"):
                cand.append(("frameToMuzzleChord", bf["value"], bf.get("confidence", WEAK)))
            chosen = None
            if cand:
                name, vec, conf = cand[0]
                # Agreement between the top two independent estimates.
                agree = None
                if len(cand) >= 2:
                    a, b = cand[0][1], cand[1][1]
                    dot = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
                    agree = math.degrees(math.acos(dot))
                if name == "sightLine" and agree is not None and agree > BORE_AGREE_DEG:
                    name, vec, conf = cand[1]
                    conf = WEAK
                chosen = (name, vec, conf, agree)
            if chosen:
                name, vec, conf, agree = chosen
                got = to_body(("__bore__", vec), True)
                if got:
                    got["evidence"] = ("%s (%s); top two independent estimates %s deg apart; %s"
                                       % (name, conf, "%.2f" % agree if agree is not None else "n/a",
                                          got["evidence"]))
                    got["confidence"] = conf if got["confidence"] != WEAK else WEAK
                    got["source"] = name
                    got["agreeDeg"] = round(agree, 3) if agree is not None else None
                    rec["boreForward"] = got
            got = to_body("muzzleOrigin", False)
            if got:
                rec["muzzleM"] = got
            got = to_body("rearGripOffset", False)
            if got:
                rec["rearGripM"] = got
            got = to_body("supportOffset", False)
            if got:
                rec["supportM"] = got
            got = to_body("barrelForward", True)
            if got:
                rec["barrelForward"] = got
            sr = src.get("supportRadius", {})
            if sr.get("value") is not None:
                v = sr["value"]
                if isinstance(v, (list, tuple)):
                    v = (sum(c * c for c in v)) ** 0.5
                rec["supportRadiusM"] = {
                    "value": round(float(v) / UNITS_PER_M, 6),
                    "confidence": sr.get("confidence", UNKNOWN)}

        models[gm.lower()] = rec

    out = {"models": sorted(models.values(), key=lambda r: r["gunModel"].lower())}
    with open(args.json, "w") as f:
        json.dump(out, f, indent=1)
    write_header(args.header, out)

    usable = [m for m in out["models"] if len(m["landmarks"]) >= 3]
    with_grip = [m for m in usable
                 if m["rearGripM"]["confidence"] in (STRONG, PROVEN)]
    keys = [m.get("vertexCount", 0) for m in usable]
    print("gun models            : %d (%d surface-key collisions)"
          % (len(out["models"]), len(keys) - len(set(keys))))
    print("with usable landmarks : %d" % len(usable))
    print("with STRONG+ rear grip: %d" % len(with_grip))
    print()
    print("%-26s %-6s %-7s %-6s %-9s %-8s %s"
          % ("model", "keyV", "surf", "lmks", "spread", "grip", "body bone"))
    print("-" * 92)
    for m in out["models"]:
        print("%-26s %-6d %-7s %-6d %-9s %-8s %s"
              % (m["gunModel"], m.get("vertexCount", 0),
                 ("%d/%s" % (m.get("keySurface", -1),
                             "+".join(str(v) for v in m["surfaces"]))),
                 len(m["landmarks"]),
                 ("%.4f" % m.get("minSpreadM", 0.0)),
                 m["rearGripM"]["confidence"][:8], m["bodyBone"]))
    return 0


def write_header(path, out):
    L = []
    L.append("// GENERATED by tools/extract_gun_landmarks.py -- do not edit by hand.")
    L.append("//")
    L.append("// One entry per gun MODEL, keyed by its BODY SURFACE's vertex count and")
    L.append("// indexed LOCALLY within that surface. The engine draws each gun surface")
    L.append("// as its own batch with its own array (measured live 2026-08-27:")
    L.append("// minIdx 0, span == surface verts, spans sum to the model total), so a")
    L.append("// model-total key could never match a real draw. A shared surface count")
    L.append("// between two models is legal: the runtime tries every matching entry")
    L.append("// and the micrometre residual picks the real one.")
    L.append("//")
    L.append("// `landmarks` are vertices bound rigidly to the model's body bone, with")
    L.append("// their bind-space positions in that bone's frame. A Kabsch solve against")
    L.append("// the submitted array gives the body's transform, and `rearGripM` --")
    L.append("// expressed in the same frame -- is then the physical pivot the weapon")
    L.append("// should rotate about. A rear grip whose confidence is below STRONG must")
    L.append("// NOT be used as a pivot; fall back to the geometric centroid and say so.")
    L.append("//")
    L.append("// boreFwd / muzzleM (2026-09-04d): the direction the shot actually")
    L.append("// follows and where it leaves the weapon, body-bone frame. Derived from")
    L.append("// the SIGHT LINE cross-checked against the muzzle tag's own rotation --")
    L.append("// never from the chord between the weapon frame's origin and the muzzle,")
    L.append("// which tilts 6.64 deg above the Thompson's sights (0.91 m high at 7.9 m).")
    L.append("")
    usable = [m for m in out["models"] if len(m["landmarks"]) >= 3]
    for m in usable:
        tag = "".join(c if c.isalnum() else "_" for c in m["gunModel"])
        L.append("static const CodvrPalmLandmark kGun_%s[] = {" % tag)
        for lm in m["landmarks"]:
            L.append("    { %5d, { %10.6ff, %10.6ff, %10.6ff } },"
                     % (lm["index"], *lm["bindM"]))
        L.append("};")
    L.append("")
    L.append("static const CodvrGunModel kGunModels[] = {")
    for m in usable:
        tag = "".join(c if c.isalnum() else "_" for c in m["gunModel"])

        def vec_and_use(ent):
            has = 1 if ent.get("confidence") in ("STRONG", "PROVEN") else 0
            v = ent.get("value") or [0.0, 0.0, 0.0]
            return v, has, ent.get("confidence", "UNKNOWN")

        rg, rgUse, rgConf = vec_and_use(m["rearGripM"])
        bo, boUse, boConf = vec_and_use(m.get("boreForward", {}))
        mz, mzUse, _mzConf = vec_and_use(m.get("muzzleM", {}))
        sp, spUse, _ = vec_and_use(m.get("supportM", {}))
        bf, bfUse, _ = vec_and_use(m.get("barrelForward", {}))
        srEnt = m.get("supportRadiusM", {})
        sr = srEnt.get("value") or 0.0
        srUse = 1 if srEnt.get("confidence") in ("STRONG", "PROVEN") else 0
        # The support anchor is usable only when the whole two-hand chain is:
        # point, slide direction and grab radius all STRONG+.
        supUse = 1 if (spUse and bfUse and srUse) else 0
        L.append("    // %s  (%s)" % (m["gunModel"], ", ".join(m["weapons"][:3])))
        L.append("    { %6d, kGun_%s, %d," % (m["vertexCount"], tag,
                                              len(m["landmarks"])))
        L.append("      { %10.6ff, %10.6ff, %10.6ff }, %d, VRWP_CONF_%s,"
                 % (rg[0], rg[1], rg[2], rgUse, rgConf))
        L.append("      { %10.6ff, %10.6ff, %10.6ff }, { %9.6ff, %9.6ff, %9.6ff }, %9.6ff, %d,"
                 % (sp[0], sp[1], sp[2], bf[0], bf[1], bf[2], sr, supUse))
        L.append("      { %9.6ff, %9.6ff, %9.6ff }, { %10.6ff, %10.6ff, %10.6ff }, %d, VRWP_CONF_%s },  // bore: %s"
                 % (bo[0], bo[1], bo[2], mz[0], mz[1], mz[2], boUse and mzUse,
                    boConf, (m.get("boreForward", {}) or {}).get("source", "none")))
    L.append("};")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


if __name__ == "__main__":
    sys.exit(main())
