#!/usr/bin/env python
"""Diagnose the 2026-09-04 hand-orientation flip.

Replicates the EXACT runtime chain that places the rendered palm --
   grip quaternion -> VRM_QuatToQuakeAxis -> (head cancel) ->
   VRM_QuakeToGLEyeAxis -> MulRows(calRows, .) -> rendered palm basis --
and compares the rendered palm's anatomical directions (knuckles k, thumb
side t, palm-out n) against the AUTHORED palm bone basis the vanilla engine
draws (from the shipped rig), which is the correct orientation.

Then it evaluates RotX/Y/Z(180) applied in the palm-local frame and reports,
for each candidate, where k / t / n end up -- so the one correction that
makes the rendered hand anatomically correct for BOTH hands, with no
left/right mirror, is picked from numbers, not Euler guessing.
"""
import json, math, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space
from extract_palm_landmarks import hand_surfaces, palm_bone, rigid_verts

UNITS_PER_M = 39.3701

# ---- exact vr_math.h replicas ------------------------------------------
def quat_to_quake_axis(q):
    x, y, z, w = q
    right = [1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w)]
    up    = [2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w)]
    back  = [2*(x*z+y*w), 2*(y*z-x*w), 1-2*(x*x+y*y)]
    fwd   = [-back[0], -back[1], -back[2]]
    left  = [-right[0], -right[1], -right[2]]
    def xr2q(v): return [-v[2], -v[0], v[1]]
    return [xr2q(fwd), xr2q(left), xr2q(up)]     # rows fwd/left/up

def quake_to_gleye_axis(a):
    def q2e(v): return [-v[1], v[2], -v[0]]
    return [q2e(a[0]), q2e(a[1]), q2e(a[2])]

def world_to_local_axis(ref, a):
    # out[k] = express row a[k] in ref's local coords (dot with each ref row)
    return [[sum(ref[j][c]*a[k][c] for c in range(3)) for j in range(3)] for k in range(3)]

def mulrows(A, B):
    # out[r][c] = sum_k A[r][k]*B[k][c]  (applies A within B's frame)
    return [[sum(A[r][k]*B[k][c] for k in range(3)) for c in range(3)] for r in range(3)]

def matvec(M, v):
    return [sum(M[r][c]*v[c] for c in range(3)) for r in range(3)]

def transpose(M):
    return [[M[c][r] for c in range(3)] for r in range(3)]

def matmul(A, B):
    return [[sum(A[r][k]*B[k][c] for k in range(3)) for c in range(3)] for r in range(3)]

def fmt(v): return "(%+.3f %+.3f %+.3f)" % (v[0], v[1], v[2])
def fmtm(M): return "  ".join(fmt(r) for r in M)

# ---- load calRows / anatomy from the generated inc ---------------------
def load_cal(path="src/vr_palm_calibration.inc"):
    txt = open(path).read()
    out = {}
    # each entry: { "rig", HAND, { rows9 }, { off3 }, { k3 }, { t3 }, { n3 } }
    pat = re.compile(r'\{\s*"([^"]+)",\s*(VRHP_TRIGGER|VRHP_SUPPORT),\s*'
                     r'\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\s*\}', re.S)
    for m in pat.finditer(txt):
        rig, hand = m.group(1), m.group(2)
        nums = [ [float(x) for x in m.group(i).replace('f','').split(',') if x.strip()] for i in range(3,8) ]
        rows9, off3, k3, t3, n3 = nums
        rows = [rows9[0:3], rows9[3:6], rows9[6:9]]
        out[(rig, hand)] = dict(rows=rows, off=off3, k=k3, t=t3, n=n3)
    return out

# ---- authored palm bone basis in EYE space (from assets) ---------------
def authored_palm_eye(fs, assets, model, weapon):
    xm = CA.parse_xmodel(fs.read("xmodel/" + model), model)
    base = xm.parts_name
    parts = CA.parse_xmodelparts(fs.read("xmodelparts/" + base), base)
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelparts/".replace("parts","surfs") + base) if False else ("xmodelsurfs/" + base), base) if False else None
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
    hs = hand_surfaces(parts, surfs)
    wraw = fs.read("weapons/sp/" + weapon)
    parts_txt = wraw.decode("latin-1").split(chr(92))
    fields = {parts_txt[i]: parts_txt[i+1] for i in range(1, len(parts_txt)-1, 2)}
    gun_parts, _ = assets.parts_for(fields["gunModel"])
    bones, index, _ = merge_skeletons(parts, gun_parts)
    anim = assets.anim(fields.get("idleAnim", ""))
    posed = pose_model_space(bones, [(anim, 0)])
    res = {}
    for surf_i, side in hs:
        surf = surfs.surfaces[surf_i]
        bi, _ = palm_bone(parts, surf)
        hi = index[parts.bones[bi].name.lower()]
        org, basis = posed[hi]   # basis rows = bone axes in MODEL (CoD-world) space
        # model(CoD world) -> GL eye is the fixed swap
        b = [list(basis[0:3]), list(basis[3:6]), list(basis[6:9])]
        eye = quake_to_gleye_axis(b)
        res["RIGHT" if side == "R" else "LEFT"] = eye
    return res

def main():
    cal = load_cal()
    fs = CA.Pk3Fs(CA.GAME_MAIN)
    assets = Assets(fs)
    authored = authored_palm_eye(fs, assets, "viewmodel_hands_new", "thompson")

    # A representative grip: the resting RIGHT controller from pair 1 of the
    # 2026-09-04 hardware log, and a synthetic "natural forward, palm-down"
    # grip. The calibration error is pose-independent, so any grip reveals it;
    # two are shown to confirm.
    grips = {
        "hw pair1 resting R": [-0.475, 0.006, 0.005, -0.880],
        "identity":           [0.0, 0.0, 0.0, 1.0],
    }
    head_id = [[1,0,0],[0,1,0],[0,0,1]]

    ROT = {
        "RotX180": [[1,0,0],[0,-1,0],[0,0,-1]],
        "RotY180": [[-1,0,0],[0,1,0],[0,0,-1]],
        "RotZ180": [[-1,0,0],[0,-1,0],[0,0,1]],
    }

    for hand, key in (("RIGHT","VRHP_TRIGGER"), ("LEFT","VRHP_SUPPORT")):
        c = cal[("viewmodel_hands_new4", key)]
        calRows, k, t, n = c["rows"], c["k"], c["t"], c["n"]
        print("\n================ %s  (bone anatomy: k=knuckles t=thumbside n=palm-out) ================" % hand)
        A = authored[hand]
        # authored anatomical directions in eye space: A rows are bone axes,
        # k/t/n are in the bone frame -> world/eye = sum comp * A_row
        def bone_to_eye(basis, v): return [sum(v[j]*basis[j][c] for j in range(3)) for c in range(3)]
        print("  AUTHORED palm-in-eye:  knuckles %s  thumb %s  palm-out %s"
              % (fmt(bone_to_eye(A,k)), fmt(bone_to_eye(A,t)), fmt(bone_to_eye(A,n))))
        for gname, q in grips.items():
            gq = quat_to_quake_axis(q)
            ge = quake_to_gleye_axis(world_to_local_axis(head_id, gq))
            base = mulrows(calRows, ge)          # rendered palm basis (eye), CURRENT
            def anat(basis):
                return (bone_to_eye(basis,k), bone_to_eye(basis,t), bone_to_eye(basis,n))
            ck, ct, cn = anat(base)
            print("  --- grip: %s ---" % gname)
            print("    CURRENT rendered:    knuckles %s  thumb %s  palm-out %s" % (fmt(ck), fmt(ct), fmt(cn)))
            for rname, R in ROT.items():
                # local (bone-frame) correction: MulRows(R, base)
                corr = mulrows(R, base)
                rk, rt, rn = anat(corr)
                print("    %s(local): knuckles %s  thumb %s  palm-out %s"
                      % (rname, fmt(rk), fmt(rt), fmt(rn)))

    print("\nEYE-SPACE READING (GL eye: +X right, +Y up, -Z forward/into screen):")
    print("  a correct right hand held forward: knuckles ~ -Z (into screen),")
    print("  palm-out(n) ~ toward the viewer/down, thumb(t) ~ toward body centre (-X for R).")
    return 0

if __name__ == "__main__":
    sys.exit(main())
