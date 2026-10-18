#!/usr/bin/env python
"""Prove the 2026-09-03 hardware refusal fingerprint = merged hand batch with
surfaces packed in SWAPPED order (surface 1 first, surface 0 second).

Fingerprint from logs/hw-2026-09-03/codvr_result.log:
  TRIG refused: rms 0.0455 m (1.7907 u), gyration 1.9397 u
  SUPP refused: rms 0.0655 m (2.5791 u), gyration 2.6618 u
  TRIG ok gyration 1.7652 u, SUPP ok 1.7352 u
"""
import math, os, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space
from orphan_residual_sweep import (parse_inc, solve_rms, gyration, load,
                                   merged_arrays, UNITS_PER_M)

def main():
    tables = parse_inc("src/vr_palm_landmarks.inc")
    lm = json.load(open("docs/palm_landmarks.json"))
    fs = CA.Pk3Fs(CA.GAME_MAIN)
    assets = Assets(fs)
    results = {}
    for weapon in ("thompson", "m1garand", "mp40", "kar98k", "sten", "bar"):
        try:
            wraw = fs.read("weapons/sp/" + weapon)
        except Exception as e:
            print("skip", weapon, e); continue
        parts_txt = wraw.decode("latin-1").split(chr(92))
        fields = {parts_txt[i]: parts_txt[i + 1] for i in range(1, len(parts_txt) - 1, 2)}
        hand_model = fields["handModel"].replace("\\", "/").split("/")[-1].lower()
        live = load(fs, assets, lm, hand_model)
        if live is None:
            print("skip", weapon, "rig", hand_model); continue
        gun_parts, _ = assets.parts_for(fields["gunModel"])
        bones, index, _ = merge_skeletons(live["parts"], gun_parts)
        anim = assets.anim(fields.get("idleAnim", ""))
        for fr in (0, 5):
            posed = pose_model_space(bones, [(anim, fr)])
            arr = merged_arrays(live, posed, index)
            if arr is None: continue
            half = 837
            normal = arr
            swapped = arr[half:] + arr[:half]
            for tname in ("kPalmTRIG", "kPalmSUPP"):
                tbl = tables[tname]; idx = [i for i, _ in tbl]
                bindU = [(b[0]*UNITS_PER_M, b[1]*UNITS_PER_M, b[2]*UNITS_PER_M) for _, b in tbl]
                for lay, a in (("normal", normal), ("swapped", swapped)):
                    obs = [a[i][0] for i in idx]
                    r = solve_rms(bindU, obs); g = gyration(obs)
                    key = "%s/%s/%s/f%d" % (weapon, hand_model, tname, fr)
                    results.setdefault(key, {})[lay] = (r, g)
                    print("%-40s %-8s rms=%.4f m  gyr=%.4f u" % (key, lay, r, g))
    return 0
if __name__ == "__main__":
    sys.exit(main())
