#!/usr/bin/env python3
"""Turn a captured GL draw stream into an identification of the viewmodel.

Input is the CSV src/vr_glsurf.cpp writes during a flat run. The job here is
NOT to decide anything by eye: it prints the evidence that a classifier can be
built on, and cross-checks it against the shipped-asset index counts recovered
from xmodelsurfs.

  python tools/analyze_draws.py runs/census/codvr_draws_census.csv
"""

import csv
import sys
from collections import Counter, defaultdict

# Asset-derived fingerprints (docs/HAND-GEOMETRY-PARTITION.md in the offline
# hardening tree; re-derived from the installed game, not invented here).
HAND_SIGS = {
    3912: "hands_new4/sten4 hand+fingers (1304 tris, 837 verts)",
    474: "hands_new4/sten4 sleeve (158 tris, 120 verts)",
    1149: "hands4 hand+fingers (383 tris, 302 verts)",
    399: "hands4 sleeve (133 tris, 93 verts)",
}


def load(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                continue
            hdr = line
            break
        rd = csv.DictReader(f, fieldnames=hdr.strip().split(","))
        for r in rd:
            rows.append(r)
    return rows


def i(r, k, d=0):
    v = r.get(k)
    if v is None or v == "":
        return d
    try:
        return int(v, 0)
    except ValueError:
        try:
            return int(float(v))
        except ValueError:
            return d


def f(r, k, d=0.0):
    try:
        return float(r.get(k, d))
    except (TypeError, ValueError):
        return d


def main(path):
    rows = load(path)
    print(f"{len(rows)} draw records from {path}\n")

    frames = sorted({i(r, "frame") for r in rows})
    print(f"frames present: {frames}")
    for fr in frames:
        fr_rows = [r for r in rows if i(r, "frame") == fr]
        scene = [r for r in fr_rows if i(r, "inScene")]
        print(f"  frame {fr}: {len(fr_rows)} draws, {len(scene)} in the 3D pass")

    # ---------------------------------------------------------- depth range
    print("\n=== depth ranges seen (the RF_DEPTHHACK discriminator) ===")
    dr = Counter((round(f(r, "dNear"), 4), round(f(r, "dFar"), 4),
                  int(bool(i(r, "inScene")))) for r in rows)
    for (near, far, insc), n in sorted(dr.items(), key=lambda kv: -kv[1]):
        tag = "scene" if insc else "2D/HUD"
        dflt = " (GL default)" if (near == 0.0 and far == 1.0) else "  <-- NARROWED"
        print(f"  [{near}, {far}] {tag:6s}  {n:5d} draws{dflt}")

    hacked = [r for r in rows
              if i(r, "inScene") and not (f(r, "dNear") == 0.0 and f(r, "dFar") == 1.0)]
    print(f"\n{len(hacked)} scene draws have a NARROWED depth range")

    # ------------------------------------------------- the depth-hacked set
    if hacked:
        print("\n=== depth-hacked draws, one frame in detail ===")
        fr = i(hacked[0], "frame")
        one = [r for r in hacked if i(r, "frame") == fr]
        print(f"  frame {fr}: {len(one)} depth-hacked draws")
        hdr = ("   seq  mode  count  span   minI   maxI   tex0  texWxH      "
               "blend cull  vertPtr    stride  cls")
        print(hdr)
        for r in one:
            print(f"  {i(r,'seq'):5d}  {i(r,'mode'):#06x} {i(r,'count'):6d} "
                  f"{i(r,'span'):5d} {i(r,'minIdx'):6d} {i(r,'maxIdx'):6d} "
                  f"{i(r,'tex0'):6d} {i(r,'tex0W'):4d}x{i(r,'tex0H'):-4d}   "
                  f"{i(r,'blend'):d}    {i(r,'cull'):d}   "
                  f"{r.get('vertPtr','')} {i(r,'vertStride'):5d}  {r.get('cls','')}")

    # --------------------------------------------------- index-count census
    print("\n=== index counts inside the depth-hacked set ===")
    cc = Counter(i(r, "count") for r in hacked)
    for cnt, n in sorted(cc.items(), key=lambda kv: -kv[1])[:40]:
        sig = HAND_SIGS.get(cnt)
        mark = f"   <== ASSET MATCH: {sig}" if sig else ""
        tris = cnt / 3.0
        print(f"  {cnt:7d} indices ({tris:8.1f} tris)  x{n:4d}{mark}")

    print("\n=== index counts in the NON-depth-hacked scene set (sanity) ===")
    world = [r for r in rows
             if i(r, "inScene") and (f(r, "dNear") == 0.0 and f(r, "dFar") == 1.0)]
    cw = Counter(i(r, "count") for r in world)
    hits = [(c, n) for c, n in cw.items() if c in HAND_SIGS]
    print(f"  {len(world)} world draws, {len(cw)} distinct index counts")
    if hits:
        print(f"  WARNING: asset fingerprints also appear in world draws: {hits}")
    else:
        print("  no hand fingerprint appears outside the depth-hacked set  (good)")

    # ---------------------------------------------------------- modelviews
    print("\n=== distinct modelview matrices in the depth-hacked set (frame 1) ===")
    if hacked:
        fr = i(hacked[0], "frame")
        one = [r for r in hacked if i(r, "frame") == fr]
        groups = defaultdict(list)
        for r in one:
            key = tuple(round(f(r, f"mv{k}"), 3) for k in range(16))
            groups[key].append(r)
        print(f"  {len(groups)} distinct modelview(s) across {len(one)} draws")
        for k, (key, rs) in enumerate(groups.items()):
            counts = Counter(i(r, "count") for r in rs)
            print(f"  --- modelview #{k}: {len(rs)} draws, index counts "
                  f"{dict(sorted(counts.items(), key=lambda kv: -kv[1])[:8])}")
            print(f"      translation = ({key[12]:9.3f}, {key[13]:9.3f}, {key[14]:9.3f})")
            rows3 = [key[0:3], key[4:7], key[8:11]]
            for rr in rows3:
                print(f"      [{rr[0]:8.4f} {rr[1]:8.4f} {rr[2]:8.4f}]")

    # --------------------------------------------------------- texture use
    print("\n=== textures bound by depth-hacked draws ===")
    tc = Counter((i(r, "tex0"), i(r, "tex0W"), i(r, "tex0H")) for r in hacked)
    for (t, w, h), n in sorted(tc.items(), key=lambda kv: -kv[1]):
        print(f"  tex {t:5d}  {w}x{h}   x{n}")

    # ---------------------------------------------------- vertex array use
    print("\n=== vertex array pointers / strides in the depth-hacked set ===")
    vp = Counter((r.get("vertPtr"), i(r, "vertStride"), i(r, "vertSize"),
                  hex(i(r, "vertType"))) for r in hacked)
    for (p, st, sz, ty), n in sorted(vp.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  ptr {p}  stride {st:3d}  size {sz}  type {ty}   x{n}")

    print("\n=== draw ordering: where the depth-hacked run sits in the frame ===")
    for fr in frames:
        fr_rows = sorted([r for r in rows if i(r, "frame") == fr],
                         key=lambda r: i(r, "seq"))
        seqs = [i(r, "seq") for r in fr_rows
                if i(r, "inScene") and not (f(r, "dNear") == 0.0 and f(r, "dFar") == 1.0)]
        allseq = [i(r, "seq") for r in fr_rows]
        if seqs:
            print(f"  frame {fr}: depth-hacked seq {min(seqs)}..{max(seqs)} "
                  f"of {min(allseq)}..{max(allseq)}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "runs/census/codvr_draws_census.csv")
