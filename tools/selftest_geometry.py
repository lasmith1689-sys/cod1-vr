#!/usr/bin/env python3
"""Deterministic tests for the asset parsers and the derived weapon geometry.

These are the checks that keep the calibration priors honest. They are not
about whether a number is right -- no headset has said yet -- they are about
whether the pipeline can produce a number it has not earned. Every one of them
was written because the pipeline actually did something wrong at some point
during this pass, or because the shipped data contains a trap that would make
it do so.

Run:  python tools/selftest_geometry.py
Exit code is non-zero on any failure.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import codasset as CA          # noqa: E402
import extract_weapons as EW   # noqa: E402
import extract_geometry as G   # noqa: E402

PASS = [0]
FAIL = []


def check(cond, fmt, *a):
    msg = fmt % a if a else fmt
    if cond:
        PASS[0] += 1
        print("    PASS  %s" % msg)
    else:
        FAIL.append(msg)
        print("    FAIL  %s" % msg)


def section(t):
    print("--- %s ---" % t)


# ---------------------------------------------------------------- container

def t_containers(fs):
    section("container structure: every file consumes to the exact byte")
    parts = fs.list("xmodelparts/")
    ok = 0
    bad_parent = 0
    non_finite = 0
    cyclic = 0
    bad_quat = 0
    for p in parts:
        nm = p.split("/", 1)[1]
        try:
            a = CA.parse_xmodelparts(fs.read(p), nm)
        except Exception:
            continue
        ok += 1
        n = len(a.bones)
        for b in a.bones:
            if b.parent >= n or b.parent < -1:
                bad_parent += 1
            for v in list(b.trans) + list(b.quat) + list(b.mins) + list(b.maxs):
                if v != v or v in (float("inf"), float("-inf")):
                    non_finite += 1
                    break
            q = b.quat
            mag = (q[0] ** 2 + q[1] ** 2 + q[2] ** 2 + q[3] ** 2) ** 0.5
            if abs(mag - 1.0) > 1e-3:
                bad_quat += 1
            seen, cur = 0, b.parent
            while cur >= 0 and seen <= n:
                cur = a.bones[cur].parent
                seen += 1
            if seen > n:
                cyclic += 1
    check(ok == len(parts), "xmodelparts: %d/%d parse to exact EOF", ok, len(parts))
    check(bad_parent == 0, "xmodelparts: no out-of-range bone parents")
    check(non_finite == 0, "xmodelparts: every transform component is finite")
    check(bad_quat == 0, "xmodelparts: every bind quaternion is unit length")
    check(cyclic == 0, "xmodelparts: every hierarchy is a forest, not a graph")

    view = [p for p in fs.list("xanim/")
            if p.split("/", 1)[1].lower().startswith("viewmodel")]
    ok = 0
    for p in view:
        try:
            CA.parse_xanim(fs.read(p), p)
            ok += 1
        except Exception:
            pass
    check(ok == len(view), "xanim: %d/%d viewmodel animations parse to exact EOF",
          ok, len(view))

    # The narrow-rotation rule is the one a naive decoder gets wrong. If maskB
    # were ignored, three bones desynchronise into every multi-bone animation,
    # so a file that uses it and still consumes exactly is the proof.
    used = 0
    for p in view[:80]:
        a = CA.parse_xanim(fs.read(p), p)
        if any(b.narrow_rot for b in a.bones):
            used += 1
    check(used > 0, "xanim: the narrow-rotation mask is exercised (%d files)", used)


# --------------------------------------------------------------- hand rigs

def t_hand_rigs(fs):
    section("hand rig: laterality survives the naming")
    for model in ("viewmodel_hands_new", "viewmodel_hands_sten", "viewmodel_hands"):
        m = CA.parse_xmodel(fs.read("xmodel/" + model), model)
        p = CA.parse_xmodelparts(fs.read("xmodelparts/" + m.parts_name), m.parts_name)

        def chain(b):
            out, cur, guard = [], b.parent, 0
            while cur >= 0 and guard < len(p.bones):
                out.append(p.bones[cur].name.lower())
                cur = p.bones[cur].parent
                guard += 1
            return out

        lh = p.by_name("bip01 l hand")
        rh = p.by_name("bip01 r hand")
        check(lh is not None and rh is not None,
              "%s: both hands present", model)
        if lh is None or rh is None:
            continue
        check("bip01 l forearm" in chain(lh) and "bip01 l upperarm" in chain(lh),
              "%s: left hand descends through the LEFT arm", model)
        check("bip01 r forearm" in chain(rh) and "bip01 r upperarm" in chain(rh),
              "%s: right hand descends through the RIGHT arm", model)
        check(lh.index != rh.index and "bip01 r forearm" not in chain(lh)
              and "bip01 l forearm" not in chain(rh),
              "%s: the two arm chains never cross", model)

        # The flag byte is the laterality authority. Every bone under the left
        # hand must carry an odd flag and every bone under the right an even
        # one -- INCLUDING `r cuff01` and `r wrist01`, which are named right
        # and parented left. A split on the name gets those two wrong.
        crossed = 0
        namefooled = 0
        for b in p.bones:
            ch = chain(b)
            if "bip01 l hand" in ch or b.name.lower() == "bip01 l hand":
                if b.flag % 2 != 1:
                    crossed += 1
                if b.name.lower().startswith("r "):
                    namefooled += 1
            elif "bip01 r hand" in ch or b.name.lower() == "bip01 r hand":
                if b.flag % 2 != 0:
                    crossed += 1
        check(crossed == 0,
              "%s: the flag byte agrees with the parent chain on every bone", model)
        if model == "viewmodel_hands_new":
            check(namefooled == 2,
                  "%s: the two right-named bones on the LEFT chain are still "
                  "there (a name split would misplace them)", model)


# ------------------------------------------------------------ derived values

def t_geometry(fs):
    section("derived geometry: no value without evidence")
    assets = G.Assets(fs)
    weapons = EW.load_sp_weapons(EW.GAME_MAIN)
    recs = {}
    for name, (_pak, f) in sorted(weapons.items()):
        cls = EW.vr_class(name, f)
        if isinstance(cls, (tuple, list)):
            cls = cls[0]
        recs[name] = G.extract_weapon(name, f, assets, cls)

    fields = ("muzzleOrigin", "barrelForward", "rearGripOffset",
              "rearGripRotation", "supportAlongAxis", "supportRadius",
              "sightOffset", "sightForward")

    mismatched = []
    for n, r in recs.items():
        for f in fields:
            v = r[f]
            has = v["value"] is not None
            claims = v["confidence"] in ("WEAK", "STRONG", "PROVEN")
            if has != claims:
                mismatched.append((n, f, v["confidence"], has))
    check(not mismatched,
          "every field has a value iff it has a confidence (%d mismatches)",
          len(mismatched))

    noev = [(n, f) for n, r in recs.items() for f in fields
            if not r[f].get("evidence")]
    check(not noev, "every field records its evidence (%d missing)", len(noev))

    # AUTO must stay AUTO where the assets are silent. The mounted turrets and
    # the ptrs41 carry no viewmodel at all, so nothing about them is derivable.
    silent = [n for n, r in recs.items() if r["status"] == "no gun model"]
    leaked = [n for n in silent
              if any(recs[n][f]["value"] is not None for f in fields)]
    check(not leaked,
          "the %d weapons with no gun model stay entirely AUTO", len(silent))

    # A barrel-less class must never acquire a barrel, however suggestive its
    # rig is. All four grenades ship a vestigial tag_flash.
    barrelless = [n for n, r in recs.items()
                  if r["vrClass"] in G.BARRELLESS_CLASSES and r["status"] == "ok"]
    bad = [n for n in barrelless
           if recs[n]["barrelForward"]["value"] is not None]
    check(not bad,
          "no barrel axis is synthesised for the %d barrel-less weapons "
          "with a rig (%s)", len(barrelless), ", ".join(bad) or "none")

    # PROVEN is reserved for agreement between independent estimates.
    unproven = []
    for n, r in recs.items():
        bf = r["barrelForward"]
        if bf["confidence"] != "PROVEN":
            continue
        ag = bf.get("agreementDeg") or {}
        if sum(1 for v in ag.values() if v <= G.MUZZLE_AGREE_DEG) < 2:
            unproven.append(n)
    check(not unproven,
          "PROVEN barrel axes all have >= 2 agreeing estimate pairs (%d bad)",
          len(unproven))

    # The steady grip must never be measured from an animation in which the
    # hands leave the weapon.
    bled = []
    for n, r in recs.items():
        for a in r.get("steadyAnims") or []:
            low = a.lower()
            if "reload" in low or "melee" in low or "lastshot" in low \
               or "rechamber" in low or "putaway" in low or "pullout" in low:
                bled.append((n, a))
    check(not bled,
          "no reload/melee/last-shot animation reached a steady grip (%d)",
          len(bled))

    # ...and the exclusion has to be load-bearing, not decorative. If the
    # excluded animations moved the hands by a millimetre, filtering them out
    # would prove nothing.
    # Measured on carried firearms only. A thrown weapon's excursion runs to
    # tens of thousands of inches because the model leaves for the horizon,
    # which is true but says nothing about whether a reload matters.
    excursions = [r["excludedSupportExcursionIn"] for r in recs.values()
                  if r.get("excludedSupportExcursionIn") is not None
                  and r["vrClass"] not in G.BARRELLESS_CLASSES]
    big = [e for e in excursions if e > 5.0]
    check(len(big) >= 10,
          "the excluded animations really do move the support hand "
          "(%d firearms by more than 5 in, worst %.1f in)",
          len(big), max(excursions) if excursions else 0.0)

    # An animation belongs to a weapon only if it drives that weapon's rig.
    # sten_engineer's emptyIdleAnim is the MK1 grenade's, which is a bug in
    # the shipped weapon files, not in ours.
    rejected = sum(len(r.get("rejectedAnims") or []) for r in recs.values())
    check(rejected >= 1,
          "cross-weapon animation references are caught and rejected (%d)",
          rejected)

    # The frame every answer is expressed in must be rigid with the gun.
    loose = [(n, r["frameRigidityIn"]) for n, r in recs.items()
             if r.get("frameRigidityIn") is not None
             and r["frameRigidityIn"] > 0.25
             and r["vrClass"] not in G.BARRELLESS_CLASSES]
    check(not loose,
          "every non-thrown weapon's answer frame is rigid with its body (%s)",
          ", ".join("%s %.2fin" % x for x in loose) or "all rigid")

    section("determinism: the same assets give the same answer")
    again = {}
    assets2 = G.Assets(CA.Pk3Fs(EW.GAME_MAIN))
    for name, (_pak, f) in sorted(weapons.items()):
        cls = EW.vr_class(name, f)
        if isinstance(cls, (tuple, list)):
            cls = cls[0]
        again[name] = G.extract_weapon(name, f, assets2, cls)
    diff = []
    for n in recs:
        for f in fields:
            if recs[n][f]["value"] != again[n][f]["value"] or \
               recs[n][f]["confidence"] != again[n][f]["confidence"]:
                diff.append((n, f))
    check(not diff, "a second extraction is identical (%d differences)", len(diff))
    return recs


# ------------------------------------------------------- generated C table

def t_table(recs):
    section("generated table: the C rows match the extraction")
    path = os.path.join(HERE, "..", "src", "vr_weapon_profile_table.inc")
    if not os.path.exists(path):
        check(False, "the generated table exists")
        return
    text = open(path).read()
    check("GENERATED by tools/gen_weapon_profiles.py" in text,
          "the table declares itself generated")
    check("VRWP_CONF_" in text, "the table carries confidence metadata")

    # A WEAK value must never appear with a STRONG or PROVEN confidence: the
    # promotion has to be impossible by construction, not by review.
    import re
    rows = re.findall(r"/\* conf \*/ \{([^}]*)\}", text)
    check(len(rows) >= 30, "every profile row carries a conf[] (%d rows)", len(rows))
    bad = [r for r in rows if len(r.split(",")) != 8]
    check(not bad, "every conf[] has one entry per field")

    # The generic fallback must stay entirely UNKNOWN, because it is what an
    # unidentified weapon gets and it must behave exactly like the shipped
    # build.
    generic = rows[0].replace(" ", "")
    check(all(c == "VRWP_CONF_UNKNOWN" for c in generic.split(",")),
          "profile 0 (generic fallback) claims nothing")

    n_weak = sum(r.count("VRWP_CONF_WEAK") for r in rows)
    n_na = sum(r.count("VRWP_CONF_NA") for r in rows)
    check(n_weak > 0 and n_na > 0,
          "the table records WEAK (%d) and N/A (%d) rather than rounding them "
          "up to STRONG", n_weak, n_na)



# --------------------------------------------------- surface partition

def t_surfaces(fs):
    section("xmodelsurfs: the renderable geometry decodes and adds up")

    paths = fs.list("xmodelsurfs/")
    ok = 0
    bad = []
    for p in paths:
        nm = p.split("/", 1)[1]
        try:
            CA.parse_xmodelsurfs(fs.read(p), nm, verts=False)
            ok += 1
        except Exception as e:
            bad.append("%s: %s" % (nm, str(e)[:90]))
    check(ok == len(paths),
          "xmodelsurfs: %d/%d files consume to exact EOF", ok, len(paths))
    for s in bad[:5]:
        print("       %s" % s)

    # The triangle count is the framing's own proof. A strip reading that
    # keeps the degenerates overshoots every declared count; one that drops
    # them reproduces them. Nothing else in the file cross-checks the strips,
    # so this is the check that the strips were read as strips at all.
    tri_ok = tri_bad = 0
    idx_bad = 0
    w_bad = 0
    surf_n = 0
    for p in paths[::7]:
        nm = p.split("/", 1)[1]
        try:
            m = CA.parse_xmodelsurfs(fs.read(p), nm)
        except Exception:
            continue
        for s in m.surfaces:
            surf_n += 1
            if len(s.tris) == s.num_tris:
                tri_ok += 1
            else:
                tri_bad += 1
            for st in s.strips:
                for i in st:
                    if not (0 <= i < s.num_verts):
                        idx_bad += 1
            if s.physiqued:
                for v in s.verts:
                    if abs(sum(w for _b, _p, w in v.weights) - 1.0) > 1e-3:
                        w_bad += 1
    check(tri_bad == 0,
          "xmodelsurfs: %d/%d sampled surfaces reproduce their declared "
          "triangle count", tri_ok, tri_ok + tri_bad)
    check(idx_bad == 0,
          "xmodelsurfs: every strip index is inside its own surface "
          "(%d surfaces checked)", surf_n)
    check(w_bad == 0,
          "xmodelsurfs: every physiqued vertex's influences sum to 1.0")


def t_partition(fs):
    section("hands: left and right are separable, and nothing is lost")

    import extract_hand_partition as HP

    for model in HP.HAND_MODELS:
        if "xmodel/" + model not in fs:
            continue
        m = HP.analyse_model(fs, model)
        t = m["totals"]
        name = m["parts"]

        check(t["verts_mixed"] == 0,
              "%s: no surface is influenced by bones from both arms", name)
        check(t["tris_crossing"] == 0,
              "%s: no triangle crosses the left/right partition", name)
        check(t["verts_left_only"] + t["verts_right_only"] == t["verts"],
              "%s: left(%d) + right(%d) == whole(%d) vertices", name,
              t["verts_left_only"], t["verts_right_only"], t["verts"])
        check(t["separable"], "%s: the model is independently drawable per side",
              name)

        # Conservation at the triangle level, and no silent loss: the strips
        # must rebuild exactly the declared count on every surface, or a split
        # that ships would quietly drop faces.
        rebuilt = sum(x["tris_rebuilt"] for x in m["surfaces"])
        declared = sum(x["tris"] for x in m["surfaces"])
        check(rebuilt == declared,
              "%s: %d triangles rebuilt from strips == %d declared",
              name, rebuilt, declared)

        lt = sum(x["tris"] for x in m["surfaces"] if x["class"] == HP.LEFT_ONLY)
        rt = sum(x["tris"] for x in m["surfaces"] if x["class"] == HP.RIGHT_ONLY)
        check(lt + rt == declared,
              "%s: the two side masks partition all %d triangles", name, declared)
        check(lt == rt, "%s: the sides are symmetric (%d triangles each)", name, lt)

        # Every vertex individually agrees with its surface's class -- so the
        # per-surface answer is not hiding a per-vertex exception.
        for x in m["surfaces"]:
            vc = x["vertex_class"]
            own = vc[x["class"]]
            check(own == x["verts"],
                  "%s surface %d: all %d vertices are %s",
                  name, x["index"], x["verts"], x["class"])

        # One hand shell and one sleeve per side: what makes "hands only" a
        # selectable option rather than a modelling job.
        parts = {}
        for x in m["surfaces"]:
            parts[(x["class"], x["part"])] = parts.get((x["class"], x["part"]), 0) + 1
        check(parts.get((HP.LEFT_ONLY, "HAND")) == 1
              and parts.get((HP.RIGHT_ONLY, "HAND")) == 1
              and parts.get((HP.LEFT_ONLY, "ARM")) == 1
              and parts.get((HP.RIGHT_ONLY, "ARM")) == 1,
              "%s: exactly one hand shell and one sleeve per side", name)

    # Surface ORDER is not a convention. If it were, code could hardcode it;
    # it is not, so this asserts the disagreement that forbids that.
    a = HP.analyse_model(fs, "viewmodel_hands_new")
    b = HP.analyse_model(fs, "viewmodel_hands")
    check(a["surfaces"][0]["class"] != b["surfaces"][0]["class"],
          "surface 0 is LEFT on one rig and RIGHT on another -- order is not "
          "a convention and must be derived per model")

    # All four surfaces share one material, so a renderer that batches by
    # material could merge them. Recording it means the eventual GL-side
    # identification is designed for the real situation.
    mats = set(x["material"] for x in a["surfaces"])
    check(len(mats) == 1,
          "all four hand surfaces share one material (%s) -- they cannot be "
          "told apart by texture binding", sorted(mats)[0])


def t_partition_matches_header(fs):
    section("hands: the baked C table still matches the installed game")

    import re
    import extract_hand_partition as HP

    hdr = os.path.join(HERE, os.pardir, "src", "vr_hands.h")
    text = open(hdr).read()

    # Pull the table back out of the header rather than trusting a comment.
    # This is the drift guard: a game patch, or an edit to either side, turns
    # into a red gate instead of a design built on a stale number.
    starts = list(re.finditer(
        r'\{\s*"(viewmodel_[a-z0-9_]+)",\s*(\d+),\s*(\d+),', text))
    check(len(starts) == 3, "vr_hands.h tabulates 3 rigs (found %d)", len(starts))

    # Each rig's rows run from its header to the next rig's header (or to the
    # end of the array). A non-greedy brace match would stop at the first row
    # and silently report one surface per rig, which is exactly the kind of
    # near-miss that makes a drift guard useless.
    baked = {}
    for i, mstart in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
        body = text[mstart.end():end]
        rows = re.findall(
            r'\{\s*VRH_SIDE_(LEFT|RIGHT),\s*VRH_PART_(HAND|ARM),\s*(\d+),\s*(\d+)\s*\}',
            body)
        baked[mstart.group(1)] = (int(mstart.group(2)), int(mstart.group(3)),
                                  [(a, b, int(v), int(t)) for a, b, v, t in rows])

    for model in HP.HAND_MODELS:
        if "xmodel/" + model not in fs:
            continue
        m = HP.analyse_model(fs, model)
        name = m["parts"]
        if name not in baked:
            check(False, "vr_hands.h has no row for %s", name)
            continue
        bones, nsurf, rows = baked[name]
        check(bones == m["bones"],
              "%s: header says %d bones, the game has %d", name, bones, m["bones"])
        check(nsurf == len(m["surfaces"]) == len(rows),
              "%s: header says %d surfaces, the game has %d", name, nsurf,
              len(m["surfaces"]))
        same = True
        for got, want in zip(rows, m["surfaces"]):
            side = "LEFT" if want["class"] == HP.LEFT_ONLY else "RIGHT"
            if (got[0] != side or got[1] != want["part"]
                    or got[2] != want["verts"] or got[3] != want["tris"]):
                same = False
        check(same, "%s: every baked surface row matches the installed asset", name)


def main():
    fs = CA.Pk3Fs()
    t_containers(fs)
    t_hand_rigs(fs)
    t_surfaces(fs)
    t_partition(fs)
    t_partition_matches_header(fs)
    recs = t_geometry(fs)
    t_table(recs)
    fs.close()
    print()
    print("=== ASSET GEOMETRY SELF TEST: %d passed, %d failed ==="
          % (PASS[0], len(FAIL)))
    for f in FAIL:
        print("   FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
