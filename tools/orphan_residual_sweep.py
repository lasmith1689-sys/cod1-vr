#!/usr/bin/env python
"""Deterministic residual sweep for the historical 0.0455 m palm refusal.

WHAT THIS ANSWERS (project-lead review 2026-08-29, section 3)
-------------------------------------------------------------
The 2026-08-28 headset run refused ~50% of primary hand draws at the solver
with a CONSTANT best residual that the logger printed as 0.0455 m (%.4f, so
the true value lies in [0.04545, 0.04555)).  The VBO shadow was found to vouch
for the zero-fill an orphaning BufferData(NULL) leaves behind; an all-zero
observed cloud produces a residual equal to the bind cloud's own radius of
gyration -- 0.044837 m for the trigger table, 1.5% short of the log.

This script exhausts the deterministic variants of that hypothesis using the
ACTUAL compiled landmark tables (src/vr_palm_landmarks.inc) and the exact
runtime solver formula (rotation-only Procrustes with the optimal translation,
proper rotation enforced, rms over the landmark set, divided by units/metre),
against the real shipped geometry posed through the real animations:

  * all points zero
  * every contiguous subset valid / remainder zero, in TABLE order and in
    VERTEX-INDEX order (the order bytes arrive in a SubData)
  * every contiguous subset current frame / remainder previous frame
  * one half previous generation, one half zero
  * SubData chunk boundaries at power-of-two byte offsets inside the real
    packed layout (tex 8 B | normal 12 B | position 12 B per vertex, measured
    from the 2026-08-27 capture: positions at byte 33480 = 1674 * 20)
  * stride/offset-shifted reads of the same packed block (+-4, +-8 bytes)
  * the NORMAL block read as positions, the TEXCOORD block read as positions
  * the previous-frame cloud with a subset zeroed

Every residual is compared against the printed fingerprint.  A hit means a
realistic partially-written pattern reproduces the historical number and the
orphan attribution becomes substantially stronger; no hit means the orphan
defect is fixed but the historical attribution stays unproven.
"""

import argparse
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space
from verify_palm_solve import kabsch

UNITS_PER_M = 39.3701
GATE_M = 0.004
# The logger prints %.4f: the historical value is any number rounding to it.
FP_LO, FP_HI = 0.04545, 0.04555
NEAR_PCT = 0.01

# The measured packed layout of the merged hand VBO (2026-08-27 capture):
# vertPtr 0x82C8 = 33480 = 1674 * 20, normPtr 0x3450 = 13392 = 1674 * 8.
BATCH = 1674
TEX_BYTES, NRM_BYTES, POS_BYTES = 8, 12, 12
TEX_OFF = 0
NRM_OFF = BATCH * TEX_BYTES            # 13392
POS_OFF = BATCH * (TEX_BYTES + NRM_BYTES)  # 33480
BUF_SIZE = BATCH * (TEX_BYTES + NRM_BYTES + POS_BYTES)  # 53568


def parse_inc(path):
    """Read the COMPILED tables, so the sweep uses exactly what the DLL uses."""
    txt = open(path).read()
    tables = {}
    for m in re.finditer(r"static const CodvrPalmLandmark (\w+)\[\] = \{(.*?)\};",
                         txt, re.S):
        name, body = m.group(1), m.group(2)
        rows = re.findall(r"\{\s*(\d+),\s*\{\s*([-\d.e]+)f,\s*([-\d.e]+)f,\s*([-\d.e]+)f\s*\}\s*\}",
                          body)
        tables[name] = [(int(i), (float(x), float(y), float(z)))
                        for i, x, y, z in rows]
    return tables


def rms_of(bind, obs, r, t):
    s = 0.0
    for pb, po in zip(bind, obs):
        x = r[0][0]*pb[0] + r[0][1]*pb[1] + r[0][2]*pb[2] + t[0]
        y = r[1][0]*pb[0] + r[1][1]*pb[1] + r[1][2]*pb[2] + t[1]
        z = r[2][0]*pb[0] + r[2][1]*pb[1] + r[2][2]*pb[2] + t[2]
        s += (x-po[0])**2 + (y-po[1])**2 + (z-po[2])**2
    return math.sqrt(s / len(bind))


def solve_rms(bind, obs):
    """Production-equivalent residual in METRES (bind and obs in units)."""
    if len(bind) != len(obs) or len(bind) < 3:
        return None
    # A degenerate (all-identical) observed cloud has a zero covariance; any
    # proper rotation is optimal and the residual is the bind gyration. The
    # SVD-based kabsch may return NaN there, so handle it explicitly the way
    # Horn's method does (identity quaternion).
    n = len(obs)
    co = [sum(p[i] for p in obs) / n for i in range(3)]
    spread = sum((p[0]-co[0])**2 + (p[1]-co[1])**2 + (p[2]-co[2])**2 for p in obs)
    if spread < 1e-18:
        cb = [sum(p[i] for p in bind) / n for i in range(3)]
        s = sum((p[0]-cb[0])**2 + (p[1]-cb[1])**2 + (p[2]-cb[2])**2 for p in bind)
        return math.sqrt(s / n) / UNITS_PER_M
    r, t = kabsch(bind, obs)
    v = rms_of(bind, obs, r, t)
    if v != v:
        return None
    return v / UNITS_PER_M


def gyration(pts):
    n = len(pts)
    c = [sum(p[i] for p in pts) / n for i in range(3)]
    return math.sqrt(sum((p[0]-c[0])**2 + (p[1]-c[1])**2 + (p[2]-c[2])**2
                         for p in pts) / n)


def load(fs, assets, lm, model_name):
    rig = next((r for r in lm["rigs"] if r.get("model", "").lower()
                == model_name.lower()), None)
    if rig is None:
        return None
    parts, base = assets.parts_for(rig["model"])
    if parts is None or not base:
        return None
    surfs = CA.parse_xmodelsurfs(fs.read("xmodelsurfs/" + base), base)
    return {"rig": rig, "parts": parts, "surfs": surfs, "base": base}


def skin_pos(weights, posed, index):
    x = y = z = 0.0
    for bone_i, p, w in weights:
        if bone_i not in posed:
            return None
        origin, basis = posed[bone_i]
        wp = CA.mat_apply(basis, p)
        x += (origin[0] + wp[0]) * w
        y += (origin[1] + wp[1]) * w
        z += (origin[2] + wp[2]) * w
    return (x, y, z)


def skin_nrm(normal, weights, posed):
    x = y = z = 0.0
    for bone_i, p, w in weights:
        if bone_i not in posed:
            return None
        _o, basis = posed[bone_i]
        wn = CA.mat_apply(basis, normal)
        x += wn[0] * w
        y += wn[1] * w
        z += wn[2] * w
    l = math.sqrt(x*x + y*y + z*z) or 1.0
    return (x/l, y/l, z/l)


def whole_surface(rigdef, surface, posed, index):
    """Skinned position+normal+uv for EVERY vertex of one surface, in engine
    units, so byte-level reads of the packed block can be reconstructed."""
    surf = rigdef["surfs"].surfaces[surface]
    out = []
    for v in surf.verts:
        p = skin_pos(v.weights, posed, index)
        n = skin_nrm(v.normal, v.weights, posed)
        if p is None or n is None:
            return None
        out.append((p, n, tuple(v.uv)))
    return out


def merged_arrays(rigdef, posed, index):
    """The merged 1674-vertex batch in file order (surface 0 then 1)."""
    a = whole_surface(rigdef, 0, posed, index)
    b = whole_surface(rigdef, 1, posed, index)
    if a is None or b is None:
        return None
    return a + b


def pack(arrays):
    """Bytes of the VBO exactly as the engine lays it out."""
    import struct
    buf = bytearray(BUF_SIZE)
    for i, (p, n, uv) in enumerate(arrays):
        struct.pack_into("<2f", buf, TEX_OFF + i*TEX_BYTES, uv[0], uv[1])
        struct.pack_into("<3f", buf, NRM_OFF + i*NRM_BYTES, *n)
        struct.pack_into("<3f", buf, POS_OFF + i*POS_BYTES, *p)
    return buf


def read_positions(buf, base, stride, indices):
    import struct
    out = []
    for i in indices:
        off = base + i * stride
        if off < 0 or off + 12 > len(buf):
            return None
        out.append(struct.unpack_from("<3f", buf, off))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=CA.GAME_MAIN)
    ap.add_argument("--landmarks", default="docs/palm_landmarks.json")
    ap.add_argument("--inc", default="src/vr_palm_landmarks.inc")
    ap.add_argument("--weapon", default="thompson")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    tables = parse_inc(args.inc)
    lm = json.load(open(args.landmarks))
    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)

    wraw = fs.read("weapons/sp/" + args.weapon)
    parts_txt = wraw.decode("latin-1").split(chr(92))
    fields = {parts_txt[i]: parts_txt[i + 1]
              for i in range(1, len(parts_txt) - 1, 2)}
    hand_model = fields["handModel"].replace("\\", "/").split("/")[-1].lower()
    live = load(fs, assets, lm, hand_model)
    if live is None:
        print("FAIL: hand rig not loadable")
        return 1
    gun_parts, _ = assets.parts_for(fields["gunModel"])
    bones, index, _ = merge_skeletons(live["parts"], gun_parts)
    print("weapon %s  hand rig %s  compiled tables: %s"
          % (args.weapon, hand_model, ", ".join(sorted(tables))))

    # Animation states, in a plausible play order so "previous frame" means
    # something: idle -> fire -> idle -> reload.
    states = []
    for fld in ("idleAnim", "fireAnim", "adsUpAnim", "reloadAnim",
                "reloadEmptyAnim", "meleeAnim"):
        nm = fields.get(fld, "")
        anim = assets.anim(nm) if nm else None
        if anim is None:
            continue
        n = anim.num_frames
        for fr in sorted(set([0, n // 3, (2 * n) // 3, max(0, n - 1)])):
            states.append((fld, fr, anim))
    print("animation states: %d" % len(states))

    # Pose every state once; keep the packed buffer and the merged arrays.
    posed_states = []
    for fld, fr, anim in states:
        posed = pose_model_space(bones, [(anim, fr)])
        arr = merged_arrays(live, posed, index)
        if arr is None:
            continue
        posed_states.append((fld, fr, arr, pack(arr)))
    print("posed states: %d" % len(posed_states))
    if len(posed_states) < 2:
        print("FAIL: need at least two posed states")
        return 1

    results = {}

    def rec(tag, vals):
        vals = [v for v in vals if v is not None]
        if vals:
            results[tag] = {"n": len(vals), "min": min(vals), "max": max(vals),
                            "mean": sum(vals) / len(vals)}

    # The two tables the live merged batch is solved with (the sten tables are
    # byte-identical to these -- established 2026-08-29).
    for tname in ("kPalmTRIG", "kPalmSUPP"):
        tbl = tables[tname]
        idx = [i for i, _ in tbl]
        bindU = [(b[0]*UNITS_PER_M, b[1]*UNITS_PER_M, b[2]*UNITS_PER_M)
                 for _, b in tbl]
        gyr = gyration(bindU) / UNITS_PER_M
        print("\n=== %s: %d landmarks, bind gyration %.6f m ===" % (tname, len(tbl), gyr))
        results["%s.bind_gyration" % tname] = {"n": 1, "min": gyr, "max": gyr, "mean": gyr}

        # Per-state observed clouds, table order.
        obs_states = []
        for fld, fr, arr, buf in posed_states:
            o = read_positions(buf, POS_OFF, POS_BYTES, idx)
            obs_states.append(o)

        # 0. correct read (control)
        rec("%s.A_correct" % tname, [solve_rms(bindU, o) for o in obs_states])
        # 1. all zero
        zero = [(0.0, 0.0, 0.0)] * len(idx)
        rec("%s.B_all_zero" % tname, [solve_rms(bindU, zero)])

        # 2. contiguous subsets valid / remainder zero, TABLE order
        best = None
        for k in range(1, len(idx)):
            for o in obs_states[:4]:
                pre = o[:k] + zero[k:]
                suf = zero[:k] + o[k:]
                for tag, cloud in (("pre", pre), ("suf", suf)):
                    v = solve_rms(bindU, cloud)
                    key = "%s.C_tableorder_%s_k%02d" % (tname, tag, k)
                    results.setdefault(key, {"n": 0, "min": 9, "max": 0, "mean": 0})
                    r = results[key]
                    r["n"] += 1; r["min"] = min(r["min"], v); r["max"] = max(r["max"], v)
                    r["mean"] += (v - r["mean"]) / r["n"]

        # 3. contiguous subsets valid / remainder zero, VERTEX-INDEX order --
        #    the order bytes actually arrive in a SubData. Cut at every
        #    landmark's position boundary.
        order = sorted(range(len(idx)), key=lambda j: idx[j])
        for k in range(1, len(idx)):
            lowset = set(order[:k])           # the k lowest-index landmarks
            for o in obs_states[:4]:
                pre = [o[j] if j in lowset else (0.0, 0.0, 0.0) for j in range(len(idx))]
                suf = [(0.0, 0.0, 0.0) if j in lowset else o[j] for j in range(len(idx))]
                for tag, cloud in (("lowvalid", pre), ("highvalid", suf)):
                    v = solve_rms(bindU, cloud)
                    key = "%s.D_indexorder_%s_k%02d" % (tname, tag, k)
                    results.setdefault(key, {"n": 0, "min": 9, "max": 0, "mean": 0})
                    r = results[key]
                    r["n"] += 1; r["min"] = min(r["min"], v); r["max"] = max(r["max"], v)
                    r["mean"] += (v - r["mean"]) / r["n"]

        # 4. subset current / remainder PREVIOUS frame (index order)
        for k in range(1, len(idx)):
            lowset = set(order[:k])
            vals = []
            for s in range(1, len(obs_states)):
                cur, prev = obs_states[s], obs_states[s-1]
                cloud = [cur[j] if j in lowset else prev[j] for j in range(len(idx))]
                vals.append(solve_rms(bindU, cloud))
            rec("%s.E_indexorder_cur_prev_k%02d" % (tname, k), vals)

        # 5. one half previous generation, one half zero (index order)
        half = len(idx) // 2
        lowset = set(order[:half])
        vals = []
        for s in range(1, len(obs_states)):
            prev = obs_states[s-1]
            cloud = [prev[j] if j in lowset else (0.0, 0.0, 0.0) for j in range(len(idx))]
            vals.append(solve_rms(bindU, cloud))
        rec("%s.F_half_prev_half_zero" % tname, vals)

        # 6. SubData chunk boundaries at byte offsets: everything below the
        #    boundary written this generation, everything above zero / prev.
        for b in (4096, 8192, 12288, 16384, 20480, 24576, 32768, 36864, 40960,
                  45056, 49152, 53248):
            for fill, label in (("zero", "zero"), ("prev", "prev")):
                vals = []
                for s in range(1, len(obs_states)):
                    cur, prev = obs_states[s], obs_states[s-1]
                    cloud = []
                    for j in range(len(idx)):
                        byte = POS_OFF + idx[j] * POS_BYTES
                        if byte + 12 <= b:
                            cloud.append(cur[j])
                        else:
                            cloud.append(prev[j] if fill == "prev" else (0.0, 0.0, 0.0))
                    vals.append(solve_rms(bindU, cloud))
                rec("%s.G_chunk_%05d_%s" % (tname, b, label), vals)

        # 7. shifted reads of the packed block
        for shift in (-8, -4, 4, 8):
            vals = []
            for fld, fr, arr, buf in posed_states:
                o = read_positions(buf, POS_OFF + shift, POS_BYTES, idx)
                if o: vals.append(solve_rms(bindU, o))
            rec("%s.H_posblock_shift%+d" % (tname, shift), vals)
        # stride-20 read (interleaved assumption on a planar buffer)
        for base, label in ((POS_OFF, "posbase"), (NRM_OFF, "nrmbase"), (0, "texbase")):
            vals = []
            for fld, fr, arr, buf in posed_states:
                o = read_positions(buf, base, 20, idx)
                if o: vals.append(solve_rms(bindU, o))
            rec("%s.I_stride20_%s" % (tname, label), vals)
        # 8. the NORMAL block / TEXCOORD block read as positions
        for base, stride, label in ((NRM_OFF, NRM_BYTES, "normals"),
                                    (TEX_OFF, TEX_BYTES, "texcoords_stride8"),
                                    (TEX_OFF, POS_BYTES, "texblock_stride12")):
            vals = []
            for fld, fr, arr, buf in posed_states:
                o = read_positions(buf, base, stride, idx)
                if o: vals.append(solve_rms(bindU, o))
            rec("%s.J_%s_as_positions" % (tname, label), vals)
        # 9. previous frame entirely (should SOLVE -- control)
        rec("%s.K_prev_frame_whole" % tname,
            [solve_rms(bindU, obs_states[s-1]) for s in range(1, len(obs_states))])
        # 10. the observed cloud's own shape signature (runtime prints this)
        rec("%s.L_shape_correct" % tname,
            [gyration(o) / UNITS_PER_M for o in obs_states])

    # ---- report ----------------------------------------------------------
    print("\n%-48s %5s %10s %10s %10s  %s"
          % ("HYPOTHESIS", "n", "min_m", "mean_m", "max_m", "verdict"))
    print("-" * 104)
    # THE FINGERPRINT IS TWO FACTS, NOT ONE. The historical refusal printed
    # 0.0455 at every one of five heartbeats spanning ~3000 refusals across
    # varied gameplay: the residual was CONSTANT to four decimals as well as
    # equal to 0.0455. A pattern whose residual sweeps through the window as
    # the animation changes (min far below, max far above) cannot have
    # produced five identical readings; it is reported as "crosses" and does
    # not count as a match. A match must be constant (max - min < 0.0005 m
    # over every animation state) AND inside the printed window.
    hits, near, crosses = [], [], []
    for tag in sorted(results):
        r = results[tag]
        const = (r["max"] - r["min"]) < 0.0005
        inwin = FP_LO <= r["mean"] <= FP_HI
        spans = (r["min"] <= FP_HI and r["max"] >= FP_LO)
        closeness = abs(r["mean"] - 0.0455) / 0.0455
        if const and inwin:
            verdict = "<<< FINGERPRINT MATCH (constant, in window)"
            hits.append(tag)
        elif const and closeness < NEAR_PCT:
            verdict = "near-constant %.2f%% off" % (closeness * 100)
            near.append(tag)
        elif spans and not const:
            verdict = "crosses the window but VARIES with animation (%.4f..%.4f)" % (r["min"], r["max"])
            crosses.append(tag)
        elif r["max"] < GATE_M:
            verdict = "solves"
        else:
            verdict = "refuses%s" % (" (constant)" if const else " (varies)")
        # keep the table readable: print the sweep rows only when notable
        if (const and (inwin or closeness < NEAR_PCT)) or not re.search(r"_k\d\d$", tag):
            print("%-48s %5d %10.5f %10.5f %10.5f  %s"
                  % (tag, r["n"], r["min"], r["mean"], r["max"], verdict))

    print("\nfingerprint window: [%.5f, %.5f] m (log printed 0.0455 with %%.4f), "
          "and CONSTANT across animation" % (FP_LO, FP_HI))
    print("CONSTANT patterns inside the window: %s"
          % (", ".join(hits) if hits else "NONE"))
    print("CONSTANT patterns within 1%% of 0.0455: %s"
          % (", ".join(near) if near else "NONE"))
    print("patterns that only CROSS the window while varying with animation "
          "(cannot be a five-heartbeat constant): %d" % len(crosses))

    if args.json_out:
        json.dump({"results": results, "hits": hits, "near": near,
                   "crosses": crosses, "window": [FP_LO, FP_HI]},
                  open(args.json_out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
