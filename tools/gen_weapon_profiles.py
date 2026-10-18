#!/usr/bin/env python3
"""Generate src/vr_weapon_profile_table.inc from the game's own data.

Two sources, both pure data files inside the shipped pk3s:

  * weapons/sp/<name>       -> class, slot, twoHanded, aimDownSight, ...
                              (tools/extract_weapons.py)
  * xanim/viewmodel_*_ADS_* -> the hip and sighted viewmodel poses
                              (tools/extract_ads_pose.py)

Weapons that share BOTH poses share one profile row: thompson and
thompson_semi are the same physical object, and nothing measurable
distinguishes them, so pretending otherwise would invent a signature that
cannot be resolved at runtime.

Every field this emits is either measured from the data or the explicit AUTO
sentinel. Nothing is guessed to fill a column.

    python tools/gen_weapon_profiles.py --out src/vr_weapon_profile_table.inc
"""

import argparse
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import extract_weapons          # noqa: E402
import extract_ads_pose         # noqa: E402
import extract_geometry         # noqa: E402
import codasset as CA           # noqa: E402

# CoD units are inches and worldscale is 39.37 units/metre, so this is exact.
IN_TO_M = 0.0254

CONF_ENUM = {
    "PROVEN": "VRWP_CONF_PROVEN",
    "STRONG": "VRWP_CONF_STRONG",
    "WEAK": "VRWP_CONF_WEAK",
    "N/A": "VRWP_CONF_NA",
    "UNKNOWN": "VRWP_CONF_UNKNOWN",
}

# Order must match the VRWP_F_* enum in vr_weapon_profile.h.
CONF_FIELDS = ("muzzleOrigin", "barrelForward", "rearGripOffset",
               "rearGripRotation", "supportAlongAxis", "supportRadius",
               "sightOffset", "sightForward")

CLASS_ENUM = {
    "unknown": "VRWC_UNKNOWN",
    "pistol": "VRWC_PISTOL",
    "rifle": "VRWC_RIFLE",
    "smg": "VRWC_SMG",
    "machinegun": "VRWC_MACHINEGUN",
    "launcher": "VRWC_LAUNCHER",
    "grenade": "VRWC_GRENADE",
    "melee": "VRWC_MELEE",
    "mounted": "VRWC_MOUNTED",
    "prop": "VRWC_PROP",
    "special": "VRWC_SPECIAL",
}

TWOHAND_ENUM = {
    "YES": "VRW2H_YES",
    "OPTIONAL": "VRW2H_OPTIONAL",
    "NO": "VRW2H_NO",
    "NO (mounted)": "VRW2H_NO",
    "NO (prop)": "VRW2H_NO",
    "UNKNOWN": "VRW2H_NO",
}


def special_bits(row, pose):
    bits = []
    if row.get("aimDownSight") != "1":
        bits.append("VRWSH_NO_ADS")
    if row.get("boltAction") == "1":
        bits.append("VRWSH_BOLT_ACTION")
    if "sniper" in row["name"] or row.get("adsOverlayShader", "").strip():
        bits.append("VRWSH_SCOPED")
    if row["vrClass"] == "grenade":
        bits.append("VRWSH_THROWN")
    if row["vrClass"] == "mounted":
        bits.append("VRWSH_ENGINE_POSED")
    return " | ".join(bits) if bits else "VRWSH_NONE"


def build_geometry(main_dir):
    """{weapon: record} from tools/extract_geometry.py, run inline.

    Regenerating rather than reading the checked-in JSON keeps the table and
    the inventory from ever disagreeing: there is one extractor and both
    outputs come from the same run.
    """
    fs = CA.Pk3Fs(main_dir)
    assets = extract_geometry.Assets(fs)
    out = {}
    for name, (_pak, fields) in sorted(
            extract_weapons.load_sp_weapons(main_dir).items()):
        cls = extract_weapons.vr_class(name, fields)
        if isinstance(cls, (tuple, list)):
            cls = cls[0]
        out[name] = extract_geometry.extract_weapon(name, fields, assets, cls)
    fs.close()
    return out


def quat_to_quake_angles(q):
    """(x,y,z,w) -> (pitch, yaw, roll) degrees, id Tech order.

    Round-tripped in the generator itself: every emitted triple is converted
    back to a matrix and compared with the source, so a convention slip fails
    generation instead of shipping a silently rotated grip.
    """
    m = CA.quat_to_mat(q)
    fwd = (m[0], m[1], m[2])
    left = (m[3], m[4], m[5])
    up = (m[6], m[7], m[8])
    xy = math.sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1])
    if xy > 1e-6:
        yaw = math.degrees(math.atan2(fwd[1], fwd[0]))
        pitch = math.degrees(math.atan2(-fwd[2], xy))
        roll = math.degrees(math.atan2(left[2], up[2]))
    else:
        yaw = 0.0
        pitch = math.degrees(math.atan2(-fwd[2], xy))
        roll = math.degrees(math.atan2(-left[0], left[1]))
    return (pitch, yaw, roll)


def angles_to_mat(pitch, yaw, roll):
    sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
    sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
    return (cp * cy, cp * sy, -sp,
            (sr * sp * cy + cr * -sy), (sr * sp * sy + cr * cy), sr * cp,
            (cr * sp * cy + -sr * -sy), (cr * sp * sy + -sr * cy), cr * cp)


def build(main_dir):
    rows = {r["name"]: r for r in
            (extract_weapons.summarise(n, pak, f)
             for n, (pak, f) in extract_weapons.load_sp_weapons(main_dir).items())}
    poses = extract_ads_pose.weapon_ads_poses(main_dir)

    # Group carried weapons by their measured pose signature. Weapons with no
    # pose (turrets, grenades) each keep their own row but carry no signature.
    families = collections.OrderedDict()
    for name in sorted(rows):
        row = rows[name]
        pose = poses.get(name)
        has_pose = bool(pose and pose.get("status") == "ok" and "adsTrans" in pose)
        if has_pose:
            key = (tuple(round(x, 4) for x in pose["hipTrans"]),
                   tuple(round(x, 4) for x in pose["adsTrans"]))
        else:
            key = ("nopose", name)
        families.setdefault(key, []).append((name, row, pose if has_pose else None))
    return families


def fvec(v):
    return "{ %10.5ff, %10.5ff, %10.5ff }" % tuple(v)


AUTO3 = "{ VRWP_AUTO, VRWP_AUTO, VRWP_AUTO }"
AUTO1 = "VRWP_AUTO"

# How far family members may differ before the shared row stops claiming to
# speak for all of them. Inches for positions, and a unit-vector distance
# (~2.9 deg) for directions.
FAMILY_TOL_IN = 0.5
FAMILY_TOL_UNIT = 0.05
UNIT_FIELDS = ("barrelForward", "sightForward")


def separation(families):
    """Minimum distance between distinct signatures, hip-only and hip+delta."""
    sigs = []
    for key, members in families.items():
        if key[0] == "nopose":
            continue
        hip, ads = key
        delta = tuple(ads[i] - hip[i] for i in range(3))
        sigs.append((members[0][0], hip, delta))

    def dist(a, b):
        return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))

    worst_hip = (1e9, "", "")
    worst_both = (1e9, "", "")
    for i in range(len(sigs)):
        for j in range(i + 1, len(sigs)):
            n1, h1, d1 = sigs[i]
            n2, h2, d2 = sigs[j]
            dh = dist(h1, h2)
            db = math.sqrt(dh * dh + dist(d1, d2) ** 2)
            if dh < worst_hip[0]:
                worst_hip = (dh, n1, n2)
            if db < worst_both[0]:
                worst_both = (db, n1, n2)
    return worst_hip, worst_both


def emit(families, main_dir, out_path, geom):
    worst_hip, worst_both = separation(families)
    lines = []
    A = lines.append
    A("// GENERATED by tools/gen_weapon_profiles.py -- do not edit by hand.")
    A("//")
    A("// Source data (pure data files inside the shipped pk3s, no executable")
    A("// is read and no process memory is touched):")
    A("//   weapons/sp/<name>        class / slot / twoHanded / aimDownSight")
    A("//   xanim/viewmodel_*_ADS_*  the hip and sighted viewmodel poses")
    A("//")
    A("// hipOffsetU / sightOffsetU are the FIRST and LAST translation keys of")
    A("// the weapon's ADS_up animation on tag_torso -- CoD units (inches), the")
    A("// viewmodel frame (+X forward, +Y left, +Z up). The decode is proven by")
    A("// the ADS_down round trip (see tools/extract_ads_pose.py --verify).")
    A("//")
    A("// The grip, muzzle, barrel and sight geometry come from")
    A("// tools/extract_geometry.py: the weapon's own bone/tag hierarchy posed")
    A("// by its own animations. Values are METRES in the WEAPON FRAME named by")
    A("// frameBone. Each carries a confidence in conf[] and NONE of it is")
    A("// hardware-verified -- these are priors that make the next headset run")
    A("// information-dense, not measurements that replace it. Anything the")
    A("// assets do not answer stays VRWP_AUTO with confidence UNKNOWN.")
    A("//")
    A("// Signature separation between distinct profiles:")
    A("//   hip offset alone   : min %.3f in  (%s vs %s)"
      % (worst_hip[0], worst_hip[1], worst_hip[2]))
    A("//   hip + hip->ADS delta: min %.3f in  (%s vs %s)"
      % (worst_both[0], worst_both[1], worst_both[2]))
    A("")
    A("static const VrWeaponProfile kVrWeaponProfiles[] = {")
    A("    // id 0: the generic fallback. This is EXACTLY the behaviour the")
    A("    // deployed build has today -- an unidentified weapon is treated as a")
    A("    // two-handed rifle, so profile routing can never make an")
    A("    // unrecognised weapon behave worse than the current candidate.")
    A('    { 0, "generic", VRWC_UNKNOWN, VRW2H_YES, VRWSH_NONE,')
    A("      %s, %s, 0," % (AUTO3, AUTO3))
    A("      %s, %s, VRWP_AUTO, VRWP_AUTO, %s, 1," % (AUTO3, AUTO3, AUTO3))
    A("      0, %s, %s, %s, %s, VRWP_AUTO," % (AUTO3, AUTO3, AUTO3, AUTO3))
    A("      /* conf */ { %s } },"
      % ", ".join(["VRWP_CONF_UNKNOWN"] * len(CONF_FIELDS)))

    ident = 1
    for key, members in families.items():
        names = [m[0] for m in members]
        row = members[0][1]
        pose = members[0][2]
        label = names[0]
        cls = CLASS_ENUM[row["vrClass"]]
        twoh = TWOHAND_ENUM[row["twoHandAllowed"]]
        bits = special_bits(row, pose)
        ads_ok = 1 if row.get("aimDownSight") == "1" else 0
        A("")
        A("    // %s%s" % (label, ("  (also: " + ", ".join(names[1:]) + ")") if len(names) > 1 else ""))
        A("    //   %s / %s / slot %s, game twoHanded=%s"
          % (row["weaponType"] or "-", row["weaponClass"] or "-",
             row["weaponSlot"] or "-", row["gameTwoHanded"] or "-"))
        A('    { %d, "%s", %s, %s, %s,' % (ident, label, cls, twoh, bits))
        if pose:
            A("      /* hip   */ %s," % fvec(pose["hipTrans"]))
            A("      /* sight */ %s, 1," % fvec(pose["adsTrans"]))
        else:
            A("      /* hip   */ %s," % AUTO3)
            A("      /* sight */ %s, 0," % AUTO3)
        g = geom.get(label) or {}
        conf = dict((f, (g.get(f) or {}).get("confidence", "UNKNOWN"))
                    for f in CONF_FIELDS)

        # A family is a set of weapons the RUNTIME cannot tell apart -- they
        # share an ADS pose signature -- but that does not make them the same
        # object. kar98k and m1garand land in one family and their muzzles are
        # four inches apart. One row has to serve both, so where the members
        # disagree the row says so instead of letting the first member speak
        # for the rest.
        disagree = {}
        for f in CONF_FIELDS:
            base = (g.get(f) or {}).get("value")
            if base is None:
                continue
            worst = 0.0
            for other in names[1:]:
                ov = ((geom.get(other) or {}).get(f) or {}).get("value")
                if ov is None:
                    worst = max(worst, FAMILY_TOL_IN * 10)
                    continue
                if isinstance(base, (int, float)):
                    worst = max(worst, abs(float(ov) - float(base)))
                else:
                    worst = max(worst, math.sqrt(sum(
                        (a - b) ** 2 for a, b in zip(ov, base))))
            if worst > (FAMILY_TOL_UNIT if f in UNIT_FIELDS else FAMILY_TOL_IN):
                disagree[f] = worst
                if conf[f] in ("PROVEN", "STRONG"):
                    conf[f] = "WEAK"
        if disagree:
            A("    //   FAMILY DISAGREEMENT -- one row serves %d weapons that the"
              % len(names))
            A("    //   runtime cannot distinguish, and these fields differ across")
            A("    //   them, so they are demoted to WEAK:")
            for f in sorted(disagree):
                A("    //     %-18s spread %.3f" % (f, disagree[f]))

        def gv3(field, scale=1.0):
            v = (g.get(field) or {}).get("value")
            if not v:
                return AUTO3
            return fvec([x * scale for x in v])

        def gv1(field, scale=1.0):
            v = (g.get(field) or {}).get("value")
            return AUTO1 if v is None else "%.6ff" % (v * scale)

        rot = (g.get("rearGripRotation") or {}).get("value")
        if rot:
            pyr = quat_to_quake_angles(tuple(rot))
            # A convention slip here rotates every grip silently, so the
            # emitted triple has to reproduce the source matrix.
            back = angles_to_mat(*pyr)
            src = CA.quat_to_mat(tuple(rot))
            err = max(abs(a - b) for a, b in zip(back, src))
            if err > 1e-4:
                raise SystemExit("%s: rearGripRotation round trip off by %g"
                                 % (label, err))
            rot_s = fvec(pyr)
        else:
            rot_s = AUTO3

        A("      /* rearGripOffsetM   */ %s," % gv3("rearGripOffset", IN_TO_M))
        A("      /* rearGripRotDeg    */ %s," % rot_s)
        A("      /* supportAlongAxisM */ %s," % gv1("supportAlongAxis", IN_TO_M))
        A("      /* supportRadiusM    */ %s," % gv1("supportRadius", IN_TO_M))
        A("      /* sightForward      */ %s," % gv3("sightForward"))
        A("      /* adsAccuracyAllowed*/ %d," % ads_ok)
        A('      /* frameBone         */ %s,'
          % (('"%s"' % g["weaponFrameBone"]) if g.get("weaponFrameBone") else "0"))
        A("      /* muzzleOriginM     */ %s," % gv3("muzzleOrigin", IN_TO_M))
        A("      /* barrelForward     */ %s," % gv3("barrelForward"))
        A("      /* supportOffsetM    */ %s," % gv3("supportOffset", IN_TO_M))
        A("      /* sightOffsetM      */ %s," % gv3("sightOffset", IN_TO_M))
        A("      /* frameRigidityM    */ %s,"
          % ("VRWP_AUTO" if g.get("frameRigidityIn") is None
             else "%.6ff" % (g["frameRigidityIn"] * IN_TO_M)))
        A("      /* conf */ { %s } },"
          % ", ".join(CONF_ENUM[conf[f]] for f in CONF_FIELDS))
        ident += 1
    A("};")
    A("")
    A("enum { VRWP_PROFILE_COUNT = %d };" % ident)

    text = "\n".join(lines) + "\n"
    with open(out_path, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text)
    return ident, worst_hip, worst_both


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=extract_weapons.GAME_MAIN)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "src",
                                                  "vr_weapon_profile_table.inc"))
    args = ap.parse_args()
    fams = build(args.main)
    geom = build_geometry(args.main)
    n, wh, wb = emit(fams, args.main, args.out, geom)
    print("profiles emitted   : %d (including the generic fallback)" % n)
    print("min hip separation : %.3f in (%s vs %s)" % wh)
    print("min hip+delta sep  : %.3f in (%s vs %s)" % wb)
    tally = collections.Counter()
    for g in geom.values():
        for f in CONF_FIELDS:
            tally[(g.get(f) or {}).get("confidence", "UNKNOWN")] += 1
    print("asset-derived values: " +
          ", ".join("%s=%d" % kv for kv in sorted(tally.items())))
    print("wrote " + os.path.normpath(args.out))


if __name__ == "__main__":
    main()
