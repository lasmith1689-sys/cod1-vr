#!/usr/bin/env python3
"""Match captured GL draw sizes against the game's own xmodelsurfs.

This is what turns "a draw of 7824 indices near the camera" into "surfaces 0
and 1 of viewmodel_hands_new4, merged into one batch". Nothing is inferred
from proximity, ordering or names -- the counts either reproduce a shipped
surface exactly or they do not.

Reuses the validated parser from the offline hardening tree (1014/1014 files
consumed to the exact byte); nothing is copied or redistributed.

  python tools/match_draw_signatures.py runs/census/codvr_draws_census.csv
"""

import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

OFFLINE = Path(r"H:\Projects\COD-VR-offline-hardening\tools")
sys.path.insert(0, str(OFFLINE))
import codasset as CA  # noqa: E402


def load_draws(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                continue
            hdr = line
            break
        for r in csv.DictReader(f, fieldnames=hdr.strip().split(",")):
            rows.append(r)
    return rows


def num(r, k, d=0):
    v = r.get(k)
    if v in (None, ""):
        return d
    try:
        return int(v, 0)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return d


def build_surface_index(fs, name_filter):
    """name -> [(indexCount, vertCount, tris), ...] in declared surface order."""
    out = {}
    for name in fs.list("xmodelsurfs/"):
        leaf = name.split("/")[-1]
        if not name_filter(leaf):
            continue
        try:
            surfs = CA.parse_xmodelsurfs(fs.read(name), leaf, verts=False)
        except Exception:
            continue
        out[leaf] = [(s.num_tris * 3, s.num_verts, s.num_tris)
                     for s in surfs.surfaces]
    return out


def main(path):
    rows = load_draws(path)

    # The draws that carry the viewmodel: 3D pass, depth range narrowed toward
    # the near plane. [1,1] is the SKY (pushed to the far plane), not the
    # viewmodel -- a distinction the first classifier got wrong.
    vm = [r for r in rows
          if num(r, "inScene") and float(r["dFar"]) < 0.999 and float(r["dNear"]) < 0.5]
    print(f"{len(rows)} draws, {len(vm)} in the near-depth-hacked (viewmodel) set")

    per_frame = defaultdict(list)
    for r in vm:
        per_frame[num(r, "frame")].append(r)
    frame = sorted(per_frame)[0]
    seq = sorted(per_frame[frame], key=lambda r: num(r, "seq"))
    wanted = [num(r, "count") for r in seq]
    print(f"frame {frame} viewmodel draw sizes (in order): {wanted}")
    print(f"frame {frame} vertex spans           (in order): "
          f"{[num(r,'span') for r in seq]}")

    fs = CA.Pk3Fs()
    print("\nindexing xmodelsurfs (viewmodel_* only) ...")
    vmods = build_surface_index(fs, lambda n: n.startswith("viewmodel_"))
    print(f"  {len(vmods)} viewmodel surf files")

    # ---------------------------------------------------------------- exact
    print("\n=== exact single-surface matches ===")
    by_count = defaultdict(list)
    for name, surfs in vmods.items():
        for si, (idx, vc, tris) in enumerate(surfs):
            by_count[idx].append((name, si, vc, tris))

    for c in wanted:
        hits = by_count.get(c, [])
        if not hits:
            print(f"  {c:6d} indices : no single surface matches")
        else:
            shown = hits[:6]
            print(f"  {c:6d} indices : {len(hits)} surface(s), e.g. " +
                  ", ".join(f"{n}#{s}(v{v})" for n, s, v, t in shown))

    # ------------------------------------------------------- merged batches
    # A batch can hold several surfaces of one model concatenated. Look for a
    # RUN of consecutive surfaces whose index counts AND vertex counts both
    # sum to what the draw carried -- two independent equalities, so a
    # coincidence has to happen twice.
    print("\n=== merged-run matches (consecutive surfaces of one model) ===")
    for r in seq:
        c = num(r, "count")
        span = num(r, "span")
        found = []
        for name, surfs in vmods.items():
            n = len(surfs)
            for a in range(n):
                ai = av = 0
                for b in range(a, n):
                    ai += surfs[b][0]
                    av += surfs[b][1]
                    if ai > c:
                        break
                    if ai == c and (span <= 0 or av == span) and b > a:
                        found.append((name, a, b, ai, av))
        if found:
            uniq = sorted({(f"{n}", a, b, ai, av) for n, a, b, ai, av in found})
            print(f"  draw seq {num(r,'seq'):3d} count {c:6d} span {span:5d} -> "
                  f"{len(uniq)} merged-run match(es)")
            for n, a, b, ai, av in uniq[:6]:
                print(f"        {n} surfaces {a}..{b}  idx {ai}  verts {av}")
        else:
            print(f"  draw seq {num(r,'seq'):3d} count {c:6d} span {span:5d} -> "
                  f"no merged run")

    # ------------------------------------------------- which weapon is this
    print("\n=== whole-model matches (all surfaces of a model, in order) ===")
    sizes = Counter(wanted)
    for name, surfs in sorted(vmods.items()):
        idxs = [s[0] for s in surfs]
        if not idxs:
            continue
        # the model's surfaces, or merged pairs of them, all present?
        total = sum(idxs)
        if total in sizes or all(x in sizes for x in idxs):
            print(f"  {name}: surfaces {idxs} total {total}   <-- consistent")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1
         else "runs/census/codvr_draws_census.csv")
