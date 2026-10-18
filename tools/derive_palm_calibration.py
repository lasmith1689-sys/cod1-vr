#!/usr/bin/env python
"""Controller grip frame -> anatomical palm-bone frame, per hand, per rig.

WHAT THIS DERIVES (project-lead brief 2026-09-03, sections 7-9)

The rendered hand is placed by carrying the authored palm BONE frame onto a
desired frame: delta = Desired * inverse(Authored). "Desired" therefore has
to be the palm bone's frame, and the controller only gives the OpenXR GRIP
frame. The two are related by a fixed, anatomical rotation and offset that
is a property of the asset and of the OpenXR grip convention -- nothing in it
is tuned, so it is derived here from the shipped skeleton and mesh and
compiled into src/vr_palm_calibration.inc.

ANATOMICAL FRAME, in either space:
    k  knuckle direction: from the palm bone origin toward the finger roots
    n  palm-out normal
    t  thumb side, t = handSign * (n x k), handSign = +1 LEFT, -1 RIGHT
       (so the triad is mirrored between hands, as hands are)

THE GRIP CONVENTION, QUOTED FROM THE SPEC (corrected AGAIN, 2026-09-04e).
OpenXR 1.0, chapter "Input", "Grip pose", VERBATIM:

  "The grip position: The user's palm centroid when holding the motion
   controller, or the centroid of the controller's grip.
   The grip orientation's +X axis: When you completely open your hand to
   form a flat 5-finger pose, the ray that is normal to the user's palm
   (away from the palm in the left hand, into the palm in the right hand).
   The grip orientation's -Z axis: When you close your hand partially (as
   if holding the motion controller), the ray that goes through the center
   of the tube formed by your non-thumb fingers, in the direction of
   little finger to thumb.
   The grip orientation's +Y axis: orthogonal to +Z and +X using the
   right-hand rule."

So, anatomically (k knuckles, t thumb side, n palm-OUT normal):
    -Z = t                      (little finger -> thumb IS the thumb side)
    +X = -n (RIGHT),  +n (LEFT) (into the palm on the right)
    +Y = Z x X = (-t) x (-/+n) = -k on BOTH hands   (the wrist side)
i.e. the grip's -Y is the pointing direction and its -Z is "up" through
the fist -- NOT "-Z forward, +Y up" as a camera frame would have it.

THE RECORD, because this has now gone wrong twice in one day and the
hardware caught both:
  * up to 2026-09-04c: "+Z little finger -> thumb, +Y along the
    metacarpals" -- Z's SIGN wrong. Rendered hands backwards and upside
    down (2026-09-04 morning). A 180 deg runtime flip made the ORIENTATION
    right (that candidate, 75E9A890, had a level bore, median +1.2 deg
    on hardware) but was never applied to the OFFSET, which is why the
    controller sat toward the weapon's body instead of in the palm.
  * 2026-09-04d: "+Y little finger -> thumb, +Z fingertips -> wrist" --
    a misreading of the spec that swapped Y and Z. The bore pitched up
    (median +51 deg, max +86 deg on hardware) and the hand read as
    "vertically up at 90 degrees".
  * 2026-09-04e (this): the spec text above, checked in a test that
    asserts the WEAPON'S BORE points along the controller's -Y.

In the palm BONE frame (what the landmark tables are expressed in): k from
the finger-root bone origins, n from the smallest principal axis of the
rigid palm shell (sign fixed by the thumb root), t from the two.

The runtime takes the grip as three rows LABELLED (fwd, left, up) =
(-Z, -X, +Y). Under the real convention those labels are wrong for a hand,
and are kept only because every consumer indexes them by position:
    row 0  (-Z)  = t         the thumb side ("up" through the fist)
    row 1  (-X)  = s * n     palm-out on the RIGHT, palm-in on the LEFT,
                             s = +1 RIGHT, -1 LEFT  (s = -handSign)
    row 2  (+Y)  = -k        the WRIST side; the hand points along -row2
and calRows[i] is bone axis i expressed in those row components, so
    calRows[i] = ( t_b[i],  s * n_b[i],  -k_b[i] )
    desiredAxis = VRM_MulRows(calRows, gripAxisRows)
The offset (grip origin = palm centroid C -> bone origin) is -C expressed in
the same row components, metres:
    offset = ( -C.t,  -s * C.n,  +C.k )
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space
from extract_palm_landmarks import hand_surfaces, palm_bone, rigid_verts

UNITS_PER_M = 39.3701

RIGS = [
    ("viewmodel_hands_new",  "thompson"),   # weapon whose idle anim poses it
    ("viewmodel_hands_sten", "sten"),
    ("viewmodel_hands",      "thompson"),   # no SP weapon selects it; bone names match
]


def v_sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def v_dot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def v_len(a):    return math.sqrt(v_dot(a, a))
def v_norm(a):
    l = v_len(a) or 1.0
    return (a[0]/l, a[1]/l, a[2]/l)
def v_cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def v_scale(a, s): return (a[0]*s, a[1]*s, a[2]*s)


def to_local(basis, origin, p):
    """Model-space point -> bone-local (rows of `basis` are the bone axes)."""
    d = v_sub(p, origin)
    return (v_dot(basis[0:3], d), v_dot(basis[3:6], d), v_dot(basis[6:9], d))


def smallest_axis(points):
    """Unit vector of least spread through the centroid (3x3 power/Jacobi)."""
    n = len(points)
    c = [sum(p[i] for p in points)/n for i in range(3)]
    m = [[0.0]*3 for _ in range(3)]
    for p in points:
        d = (p[0]-c[0], p[1]-c[1], p[2]-c[2])
        for r in range(3):
            for cc in range(3):
                m[r][cc] += d[r]*d[cc]
    # Jacobi eigen-decomposition.
    a = [row[:] for row in m]
    v = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    for _ in range(64):
        p, q, off = 0, 1, 0.0
        for i in range(3):
            for j in range(i+1, 3):
                if abs(a[i][j]) > off: off, p, q = abs(a[i][j]), i, j
        if off < 1e-15: break
        th = (a[q][q]-a[p][p]) / (2*a[p][q])
        t = (1.0 if th >= 0 else -1.0) / (abs(th) + math.sqrt(th*th+1))
        cs = 1/math.sqrt(t*t+1); sn = t*cs
        for k in range(3):
            akp, akq = a[k][p], a[k][q]
            a[k][p] = cs*akp - sn*akq; a[k][q] = sn*akp + cs*akq
        for k in range(3):
            apk, aqk = a[p][k], a[q][k]
            a[p][k] = cs*apk - sn*aqk; a[q][k] = sn*apk + cs*aqk
        for k in range(3):
            vkp, vkq = v[k][p], v[k][q]
            v[k][p] = cs*vkp - sn*vkq; v[k][q] = sn*vkp + cs*vkq
    ev = [a[i][i] for i in range(3)]
    i = ev.index(min(ev))
    return v_norm((v[0][i], v[1][i], v[2][i])), tuple(c)


def derive(fs, assets, model_name, weapon):
    xm = CA.parse_xmodel(fs.read("xmodel/" + model_name), model_name)
    base = xm.parts_name
    parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + base), base)
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
    hs = hand_surfaces(parts, surfs)
    if len(hs) != 2:
        raise SystemExit("%s: expected 2 hand surfaces" % model_name)

    # Pose the rig with a real animation so the finger roots have positions
    # (the viewmodel bind pose is all-zero; positions live in the anims).
    wraw = fs.read("weapons/sp/" + weapon)
    parts_txt = wraw.decode("latin-1").split(chr(92))
    fields = {parts_txt[i]: parts_txt[i+1] for i in range(1, len(parts_txt)-1, 2)}
    gun_parts, _ = assets.parts_for(fields["gunModel"])
    bones, index, _ = merge_skeletons(parts, gun_parts)
    anim = assets.anim(fields.get("idleAnim", ""))
    posed = pose_model_space(bones, [(anim, 0)])

    out = []
    for surf_i, asset_side in hs:
        surf = surfs.surfaces[surf_i]
        bi, _n = palm_bone(parts, surf)
        hand_name = parts.bones[bi].name
        hi = index[hand_name.lower()]
        h_org, h_basis = posed[hi]
        side = asset_side  # 'R' -> rendered RIGHT (trigger), 'L' -> LEFT
        hand_sign = -1.0 if side == "R" else 1.0
        prefix = "bip01 %s finger" % side.lower()
        roots = {}
        for nm, bidx in index.items():
            if nm.startswith(prefix) and len(nm) == len(prefix) + 1:
                roots[nm[-1]] = to_local(h_basis, h_org, posed[bidx][0])
        if not all(d in roots for d in "01234"):
            raise SystemExit("%s: finger roots missing for %s: %r" % (model_name, side, roots))
        # k: mean direction to the four finger roots (the metacarpal heads).
        k = v_norm(tuple(sum(roots[d][i] for d in "1234")/4.0 for i in range(3)))
        # n: least-spread axis of the rigid palm shell, in bone space.
        rv = rigid_verts(surf, bi)
        n_dir, centroid = smallest_axis([p for _i, p in rv])
        # Sign: the thumb root must lie on the t = handSign*(n x k) side.
        t0 = roots["0"]
        t_cand = v_scale(v_cross(n_dir, k), hand_sign)
        if v_dot(t_cand, t0) < 0:
            n_dir = v_scale(n_dir, -1.0)
        # Orthogonalise: k perpendicular to n, then t.
        k = v_norm(v_sub(k, v_scale(n_dir, v_dot(k, n_dir))))
        t = v_norm(v_scale(v_cross(n_dir, k), hand_sign))
        n = n_dir
        # Rows: bone axis i expressed in the grip rows (-Z, -X, +Y), with
        # -Z = t, -X = s*n, +Y = -k (see the module docstring; s = -handSign
        # is the palm-out sign in row 1).
        s = -hand_sign
        rows = []
        for i in range(3):
            rows.append((t[i], s * n[i], -k[i]))
        # Offset: grip origin (palm centroid C) -> bone origin = -C, in the
        # same row components, metres.
        Cm = v_scale(centroid, 1.0 / UNITS_PER_M)
        off = (-v_dot(Cm, t), -s * v_dot(Cm, n), v_dot(Cm, k))
        # Determinant check: rows must be a proper rotation.
        r = rows
        det = (r[0][0]*(r[1][1]*r[2][2]-r[1][2]*r[2][1])
             - r[0][1]*(r[1][0]*r[2][2]-r[1][2]*r[2][0])
             + r[0][2]*(r[1][0]*r[2][1]-r[1][1]*r[2][0]))
        out.append({
            "surface": surf_i, "assetSide": side,
            "rendersAs": "RIGHT" if side == "R" else "LEFT",
            "palmBone": hand_name, "handSign": hand_sign,
            "kBone": k, "tBone": t, "nBone": n,
            "fingerRootsBoneU": {d: roots[d] for d in "01234"},
            "palmCentroidBoneM": Cm,
            "rows": rows, "offsetM": off, "det": det,
        })
    return {"model": model_name, "parts": base, "weaponForPose": weapon, "hands": out}


def write_inc(path, docs):
    L = []
    L.append("// GENERATED by tools/derive_palm_calibration.py -- do not edit by hand.")
    L.append("//")
    L.append("// Controller GRIP frame -> anatomical PALM BONE frame, per hand, per rig")
    L.append("// (project-lead brief 2026-09-03, sections 7-9). Derived from the shipped")
    L.append("// skeleton (finger roots) and mesh (rigid palm shell) against the OpenXR")
    L.append("// grip convention AS SPECIFIED (+X palm normal, -Z little finger ->")
    L.append("// thumb, +Y = Z x X), so in the grip rows labelled (fwd,left,up) =")
    L.append("// (-Z,-X,+Y): row0 = thumb side, row1 = palm-out (RIGHT) / palm-in")
    L.append("// (LEFT), row2 = the WRIST side -- the hand points along -row2. Both")
    L.append("// earlier tables mis-stated this and the hardware caught both (see")
    L.append("// the tool's docstring); the 2026-09-04e test asserts the weapon's bore")
    L.append("// along the controller's pointing axis.")
    L.append("// grip convention; nothing here is tuned. See the tool's docstring for")
    L.append("// the frames. rows: bone axis i expressed in the grip's Quake rows")
    L.append("// (fwd, left, up); desiredAxis = VRM_MulRows(rows, gripAxis). offsetM:")
    L.append("// grip origin (palm centroid) -> bone origin, grip (fwd,left,up), metres.")
    L.append("")
    L.append("struct CodvrPalmCal {")
    L.append("    const char* rig;        // parts name, matches kHandRigDefs order")
    L.append("    int         hand;       // VRHP_TRIGGER (RIGHT) / VRHP_SUPPORT (LEFT)")
    L.append("    float       rows[9];")
    L.append("    float       offsetM[3];")
    L.append("    float       kBone[3];   // anatomical directions in the bone frame")
    L.append("    float       tBone[3];")
    L.append("    float       nBone[3];")
    L.append("};")
    L.append("")
    L.append("static const CodvrPalmCal kPalmCal[] = {")
    for d in docs:
        for h in sorted(d["hands"], key=lambda x: 0 if x["rendersAs"] == "RIGHT" else 1):
            hand = "VRHP_TRIGGER" if h["rendersAs"] == "RIGHT" else "VRHP_SUPPORT"
            L.append("    // %s %s (%s), det %+.4f" % (d["parts"], h["rendersAs"], h["palmBone"], h["det"]))
            L.append("    { \"%s\", %s," % (d["parts"], hand))
            L.append("      { %s }," % ", ".join("%.6ff" % c for r in h["rows"] for c in r))
            L.append("      { %s }," % ", ".join("%.6ff" % c for c in h["offsetM"]))
            L.append("      { %s }," % ", ".join("%.6ff" % c for c in h["kBone"]))
            L.append("      { %s }," % ", ".join("%.6ff" % c for c in h["tBone"]))
            L.append("      { %s } }," % ", ".join("%.6ff" % c for c in h["nBone"]))
    L.append("};")
    L.append("")
    open(path, "w").write("\n".join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--out", default="src/vr_palm_calibration.inc")
    ap.add_argument("--json-out", default="docs/palm_calibration.json")
    args = ap.parse_args()
    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)
    docs = []
    for model, weapon in RIGS:
        d = derive(fs, assets, model, weapon)
        docs.append(d)
        for h in d["hands"]:
            print("%-22s %-5s bone %-14s det %+.4f" % (d["parts"], h["rendersAs"], h["palmBone"], h["det"]))
            print("   k_b %s  t_b %s  n_b %s" % (
                " ".join("%+.3f" % c for c in h["kBone"]),
                " ".join("%+.3f" % c for c in h["tBone"]),
                " ".join("%+.3f" % c for c in h["nBone"])))
            print("   rows %s" % "  ".join("(%+.3f %+.3f %+.3f)" % r for r in h["rows"]))
            print("   offsetM (fwd,left,up) %s   centroid_bone_m %s" % (
                " ".join("%+.4f" % c for c in h["offsetM"]),
                " ".join("%+.4f" % c for c in h["palmCentroidBoneM"])))
            print("   finger roots (bone u): " + "  ".join(
                "%s=(%+.2f %+.2f %+.2f)" % (dd, *h["fingerRootsBoneU"][dd]) for dd in "01234"))
    write_inc(args.out, docs)
    json.dump(docs, open(args.json_out, "w"), indent=1, default=lambda o: list(o))
    print("wrote", args.out, "and", args.json_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
