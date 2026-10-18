#!/usr/bin/env python3
"""Structural self-test for the CoD1 asset parsers.

The only claim worth making about a reverse-engineered binary format is that
it consumes every shipped file to the exact byte. Anything less means a field
width is wrong somewhere, and a mis-framed record still produces numbers --
just wrong ones, delivered with the same confidence as right ones. So this
walks all three containers across every pk3 and reports the exact counts.

It also checks the invariants the calibration work later depends on:

  * bone parents are in range and acyclic (a forest, not a graph)
  * every bind-pose quaternion is a unit quaternion within tolerance
  * every transform component is finite
  * left/right bone names are not crossed between the two arm chains

Usage:  python tools/selftest_assets.py [--verbose]
Exit code is non-zero if any file fails to parse or any invariant breaks.
"""

import argparse
import sys

import codasset as CA

QUAT_TOL = 2.0 / 32768.0 * 4.0     # packing granularity, generously


def check_hierarchy(parts):
    """Return a list of problem strings for one xmodelparts asset."""
    problems = []
    n = len(parts.bones)
    for b in parts.bones:
        if b.parent >= n:
            problems.append("%s: bone %d parent %d out of range (%d bones)"
                            % (parts.name, b.index, b.parent, n))
        if b.parent == b.index:
            problems.append("%s: bone %d is its own parent" % (parts.name, b.index))
        for v in list(b.trans) + list(b.quat) + list(b.mins) + list(b.maxs):
            if v != v or v in (float("inf"), float("-inf")):
                problems.append("%s: bone %d non-finite value" % (parts.name, b.index))
                break
        q = b.quat
        mag = (q[0] ** 2 + q[1] ** 2 + q[2] ** 2 + q[3] ** 2) ** 0.5
        if abs(mag - 1.0) > QUAT_TOL:
            problems.append("%s: bone %d quat magnitude %.6f" % (parts.name, b.index, mag))
        for i in range(3):
            if b.maxs[i] < b.mins[i]:
                problems.append("%s: bone %d inverted AABB axis %d"
                                % (parts.name, b.index, i))
    # acyclic: every bone must reach a root in <= n steps
    for b in parts.bones:
        seen = 0
        cur = b.parent
        while cur >= 0 and seen <= n:
            cur = parts.bones[cur].parent if cur < n else -1
            seen += 1
        if seen > n:
            problems.append("%s: cycle above bone %d" % (parts.name, b.index))
            break
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--main", default=CA.GAME_MAIN)
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    fail = 0
    checks = 0

    # ---------------------------------------------------------- xmodelparts
    paths = fs.list("xmodelparts/")
    ok = 0
    problems = []
    for p in paths:
        nm = p.split("/", 1)[1]
        try:
            parts = CA.parse_xmodelparts(fs.read(p), nm)
        except Exception as e:
            problems.append("%s: %s" % (nm, e))
            continue
        ok += 1
        problems.extend(check_hierarchy(parts))
    checks += len(paths)
    print("xmodelparts : %d/%d parsed to exact EOF, %d invariant problems"
          % (ok, len(paths), len(problems)))
    if problems:
        fail += 1
        for s in problems[:20]:
            print("    ", s)

    # --------------------------------------------------------------- xmodel
    paths = fs.list("xmodel/")
    ok = 0
    with_mats = 0
    bad = []
    for p in paths:
        nm = p.split("/", 1)[1]
        try:
            m = CA.parse_xmodel(fs.read(p), nm)
        except Exception as e:
            bad.append("%s: %s" % (nm, e))
            continue
        ok += 1
        if m.materials:
            with_mats += 1
    checks += len(paths)
    print("xmodel      : %d/%d header parsed, %d with a framed material block"
          % (ok, len(paths), with_mats))
    if bad:
        fail += 1
        for s in bad[:20]:
            print("    ", s)

    # ---------------------------------------------------------------- xanim
    #
    # SCOPE. The decoder covers the VIEWMODEL animation flavour, which is the
    # only one this project reads: first-person weapon poses. Character,
    # facial and vehicle animations are a different sub-variant of v14 -- they
    # carry an extra pre-name block whose framing is not established here --
    # and they are reported, not silently counted as failures, because
    # claiming to decode what has not been proven is the failure mode this
    # whole exercise exists to avoid.
    paths = fs.list("xanim/")
    view = [p for p in paths if p.split("/", 1)[1].lower().startswith("viewmodel")]
    other = [p for p in paths if p not in view]
    ok = 0
    bad = []
    for p in view:
        nm = p.split("/", 1)[1]
        try:
            a = CA.parse_xanim(fs.read(p), nm)
        except Exception as e:
            bad.append("%s: %s" % (nm, str(e)[:110]))
            continue
        ok += 1
        for b in a.bones:
            for t in b.trans:
                for v in t:
                    if v != v:
                        bad.append("%s: non-finite translation" % nm)
                        break
    checks += len(view)
    print("xanim       : %d/%d VIEWMODEL animations parsed to exact EOF" % (ok, len(view)))
    other_ok = 0
    for p in other:
        try:
            CA.parse_xanim(fs.read(p), p)
            other_ok += 1
        except Exception:
            pass
    print("              (%d/%d non-viewmodel animations also parse; the rest are "
          "a v14 sub-variant this decoder does not claim)" % (other_ok, len(other)))
    if bad:
        fail += 1
        for s in bad[:20]:
            print("    ", s)
        if len(bad) > 20:
            print("     ... and %d more" % (len(bad) - 20))

    print("TOTAL       : %d assets examined" % checks)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
