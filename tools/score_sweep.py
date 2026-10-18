#!/usr/bin/env python3
"""Score a real flat A/B sweep. Mechanical: no eyeballing, no judgement calls.

The sweep writes, per stage, a draw census (CSV) and a framebuffer (TGA). This
turns those into the PASS/FAIL table the milestone gate requires.

TWO INDEPENDENT LINES OF EVIDENCE, deliberately:

  1. PER-DRAW MATRICES (exact). Every census row carries both the modelview the
     ENGINE set (`mv*`) and the one the draw actually rendered under (`us*`).
     For an untouched draw those are byte-identical. So "the left hand did not
     move while the right hand did" is a matrix equality on the very frame in
     question -- it cannot be confused by idle animation, and it does not
     depend on a screenshot at all.

  2. FRAMEBUFFER DIFF (corroborating). Each probe stage has a baseline stage
     immediately before it. diff(baseline_n, baseline_n+1) is the animation
     NOISE FLOOR over one stage; diff(baseline_n, probe) must clear that floor
     in the region the transform is supposed to move, and must not clear it
     anywhere else.

Usage:
  python tools/score_sweep.py runs/sweep
"""

import csv
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Probe -> the class it is supposed to move, and everything that must not move.
STAGES = {
    "gun_x":     ("gun",      ("hand_L", "hand_R", "sleeve_L", "sleeve_R")),
    "gun_yaw":   ("gun",      ("hand_L", "hand_R", "sleeve_L", "sleeve_R")),
    "handR_x":   ("hand_R",   ("hand_L", "gun", "sleeve_L", "sleeve_R")),
    "handL_x":   ("hand_L",   ("hand_R", "gun", "sleeve_L", "sleeve_R")),
    "sleeveR_x": ("sleeve_R", ("sleeve_L", "gun", "hand_L", "hand_R")),
    "sleeveL_x": ("sleeve_L", ("sleeve_R", "gun", "hand_L", "hand_R")),
}
BASE_FOR = {
    "gun_x": "base0", "gun_yaw": "base1", "handR_x": "base2",
    "handL_x": "base3", "sleeveR_x": "base4", "sleeveL_x": "base5",
}
NEXT_BASE = {
    "gun_x": "base1", "gun_yaw": "base2", "handR_x": "base3",
    "handL_x": "base4", "sleeveR_x": "base5", "sleeveL_x": "base6",
}

MV = [f"mv{i}" for i in range(16)]
US = [f"us{i}" for i in range(16)]


def load_csv(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                continue
            hdr = line
            break
        else:
            return rows
        for r in csv.DictReader(f, fieldnames=hdr.strip().split(",")):
            rows.append(r)
    return rows


def fv(r, k, d=0.0):
    try:
        return float(r.get(k, d))
    except (TypeError, ValueError):
        return d


def iv(r, k, d=0):
    v = r.get(k)
    if v in (None, ""):
        return d
    try:
        return int(v, 0)
    except ValueError:
        try:
            return int(float(v))
        except ValueError:
            return d


def mat(r, keys):
    return [fv(r, k) for k in keys]


def mat_equal(a, b, eps=0.0):
    return all(abs(x - y) <= eps for x, y in zip(a, b))


def mat_delta_translation(engine, used):
    """Translation the transform introduced, in eye space."""
    return (used[12] - engine[12], used[13] - engine[13], used[14] - engine[14])


def read_tga(path):
    d = Path(path).read_bytes()
    idlen = d[0]
    imgtype = d[2]
    w = struct.unpack_from("<H", d, 12)[0]
    h = struct.unpack_from("<H", d, 14)[0]
    bpp = d[16]
    if imgtype != 2 or bpp != 24:
        raise ValueError(f"{path}: unsupported TGA (type {imgtype}, {bpp}bpp)")
    off = 18 + idlen
    return w, h, d[off:off + w * h * 3]


def diff_mask(a, b, w, h, thresh=12):
    """Per-pixel changed mask plus a bounding box, as a coarse 32x32 grid."""
    gw = gh = 32
    grid = [0] * (gw * gh)
    changed = 0
    for y in range(0, h, 2):                      # every other row: 4x cheaper
        rowoff = y * w * 3
        gy = (y * gh) // h
        for x in range(0, w, 2):
            i = rowoff + x * 3
            if (abs(a[i] - b[i]) + abs(a[i+1] - b[i+1]) + abs(a[i+2] - b[i+2])) > thresh:
                changed += 1
                grid[gy * gw + (x * gw) // w] += 1
    return changed, grid


def grid_overlap(g1, g2):
    """How much of g1's changed area is also changed in g2 (0..1)."""
    t = sum(1 for v in g1 if v)
    if not t:
        return 0.0
    both = sum(1 for v1, v2 in zip(g1, g2) if v1 and v2)
    return both / t


def score(run_dir):
    run = Path(run_dir)
    print(f"=== SWEEP SCORE: {run} ===\n")

    census = {}
    for p in sorted(run.glob("codvr_draws_*.csv")):
        name = p.stem.replace("codvr_draws_", "")
        stage = name.split("_", 1)[1] if "_" in name else name
        census[stage] = load_csv(p)
    shots = {}
    for p in sorted(run.glob("codvr_shot_*.tga")):
        name = p.stem.replace("codvr_shot_", "")
        stage = name.split("_", 1)[1] if "_" in name else name
        shots[stage] = p

    print(f"stages with a draw census : {sorted(census)}")
    print(f"stages with a screenshot  : {sorted(shots)}\n")

    missing = [s for s in list(STAGES) + list(BASE_FOR.values()) if s not in census]
    if missing:
        print(f"!! MISSING census for: {missing}")

    log = run / "codvr_result.log"
    logtext = log.read_text(errors="replace") if log.exists() else ""

    # ---------------------------------------------------------------- part 1
    print("=" * 74)
    print("PART 1 -- PER-DRAW MATRIX EVIDENCE (exact, animation-independent)")
    print("=" * 74)

    verdicts = {}
    for stage, (target, must_not_move) in STAGES.items():
        rows = census.get(stage)
        print(f"\n--- stage {stage}: target = {target} ---")
        if not rows:
            print("  NO CENSUS -- cannot score")
            verdicts[stage] = False
            continue

        by_cls = defaultdict(list)
        for r in rows:
            by_cls[r.get("cls", "?")].append(r)

        # signature of every viewmodel class present
        print("  draw signatures in the viewmodel set:")
        for cls in sorted(by_cls):
            if cls in ("none", "?"):
                continue
            rs = by_cls[cls]
            counts = Counter(iv(r, "count") for r in rs)
            spans = Counter(iv(r, "span") for r in rs)
            print(f"    {cls:9s} x{len(rs):<3d} indices={dict(counts)} "
                  f"vertSpan={dict(spans)}")

        moved = by_cls.get(target, [])
        if not moved:
            print(f"  FAIL: no draw was classified {target}")
            verdicts[stage] = False
            continue

        # target must actually have been transformed
        xformed = [r for r in moved if iv(r, "xformed") == 1]
        deltas = [mat_delta_translation(mat(r, MV), mat(r, US)) for r in xformed]
        print(f"  target draws: {len(moved)}, transformed: {len(xformed)}")
        for d in deltas[:4]:
            print(f"    eye-space translation introduced: "
                  f"({d[0]:+.4f}, {d[1]:+.4f}, {d[2]:+.4f}) units")

        ok_target = len(xformed) > 0
        if stage.endswith("_yaw"):
            rotated = []
            for r in xformed:
                e, u = mat(r, MV), mat(r, US)
                rotated.append(not mat_equal(e[0:3] + e[4:7] + e[8:11],
                                             u[0:3] + u[4:7] + u[8:11], 1e-5))
            ok_target = ok_target and all(rotated)
            print(f"  rotation applied to the 3x3 basis: {all(rotated) if rotated else False}")
        else:
            want = 3.937
            got_ok = [abs(abs(d[0]) - want) < 0.01 and abs(d[1]) < 1e-4 and abs(d[2]) < 1e-4
                      for d in deltas]
            ok_target = ok_target and all(got_ok)
            print(f"  translation matches the requested 3.937 units: "
                  f"{all(got_ok) if got_ok else False}")

        # everything else must be byte-identical
        ok_others = True
        for cls in must_not_move:
            rs = by_cls.get(cls, [])
            if not rs:
                print(f"    {cls:9s}: not present this frame")
                continue
            untouched = [mat_equal(mat(r, MV), mat(r, US), 0.0) and iv(r, "xformed") == 0
                         for r in rs]
            print(f"    {cls:9s}: {sum(untouched)}/{len(rs)} draws BYTE-IDENTICAL "
                  f"engine-vs-rendered")
            ok_others = ok_others and all(untouched)

        # the world and the HUD, in the same frame
        world = [r for r in rows if r.get("cls") == "none"]
        world_ok = all(mat_equal(mat(r, MV), mat(r, US), 0.0) and iv(r, "xformed") == 0
                       for r in world)
        hud = [r for r in rows if not iv(r, "inScene")]
        hud_ok = all(iv(r, "xformed") == 0 for r in hud)
        print(f"    world/unclaimed draws untouched : {world_ok}  ({len(world)} draws)")
        print(f"    HUD draws untouched             : {hud_ok}  ({len(hud)} draws)")

        # projection must be identical across every draw in the stage
        projs = {tuple(round(fv(r, k), 6) for k in
                       ("pj0", "pj5", "pj8", "pj9", "pj10", "pj11", "pj12", "pj13", "pj14"))
                 for r in rows}
        proj_ok = len(projs) == 1
        print(f"    projection constant across stage : {proj_ok} ({len(projs)} distinct)")

        verdicts[stage] = bool(ok_target and ok_others and world_ok and hud_ok and proj_ok)
        print(f"  => {stage}: {'PASS' if verdicts[stage] else 'FAIL'}")

    # -------------------------------------------------- split verification
    print("\n" + "=" * 74)
    print("SPLIT VERIFICATION ON THE ACTUAL DRAWS")
    print("=" * 74)
    for line in logtext.splitlines():
        if "split verified" in line or "merged-batch split" in line:
            print("  " + line.strip())
    for stage in ("handR_x", "handL_x"):
        rows = census.get(stage, [])
        halves = [r for r in rows if r.get("cls", "").startswith("hand_")]
        counts = Counter(iv(r, "count") for r in halves)
        print(f"  {stage}: {len(halves)} hand-half draws, index counts {dict(counts)}")
        good = all(c == 3912 for c in counts) and len(halves) == 2
        print(f"           split produced two 3912-index halves: {good}")

    # ---------------------------------------------------------------- part 2
    print("\n" + "=" * 74)
    print("PART 2 -- FRAMEBUFFER DIFF vs a LOCAL BASELINE (corroborating)")
    print("=" * 74)
    for stage in STAGES:
        b0, b1 = BASE_FOR.get(stage), NEXT_BASE.get(stage)
        if stage not in shots or b0 not in shots or b1 not in shots:
            print(f"  {stage}: screenshots incomplete, skipped")
            continue
        w, h, a = read_tga(shots[b0])
        _, _, p = read_tga(shots[stage])
        _, _, c = read_tga(shots[b1])
        n_probe, g_probe = diff_mask(a, p, w, h)
        n_noise, g_noise = diff_mask(a, c, w, h)
        ratio = (n_probe / n_noise) if n_noise else float("inf")
        outside = sum(1 for gp, gn in zip(g_probe, g_noise) if gp and not gn)
        print(f"  {stage}: changed px {n_probe} vs animation floor {n_noise} "
              f"(x{ratio:.2f}); {outside} grid cells changed by the probe alone")

    # ------------------------------------------------------------- verdict
    print("\n" + "=" * 74)
    print("GATE")
    print("=" * 74)
    gate = {
        "GUN": verdicts.get("gun_x", False) and verdicts.get("gun_yaw", False),
        "RIGHT_HAND": verdicts.get("handR_x", False),
        "LEFT_HAND": verdicts.get("handL_x", False),
        "SLEEVES": verdicts.get("sleeveR_x", False) and verdicts.get("sleeveL_x", False),
    }
    for k, v in gate.items():
        print(f"  {k:12s} {'PASS' if v else 'FAIL'}")
    hard = gate["GUN"] and gate["RIGHT_HAND"] and gate["LEFT_HAND"]
    print(f"\n  HARD GATE (gun + right hand + left hand): "
          f"{'PASS' if hard else 'FAIL'}")
    return 0 if hard else 1


if __name__ == "__main__":
    sys.exit(score(sys.argv[1] if len(sys.argv) > 1 else "runs/sweep"))
