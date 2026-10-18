#!/usr/bin/env python
"""Reload profiles for the physical-reload gesture machine.

WHY THIS EXISTS
---------------
The offline-hardening tree's weapon profiles carry grip, barrel and sight
geometry but say nothing about reloading. A physical reload needs three things
the shipped weapon files half-answer and the shipped ANIMATIONS answer fully:

  1. WHAT KIND of reload this weapon has -- one magazine swapped, a clip
     pressed in from above, rounds fed one at a time, or none at all.
  2. WHERE the support hand goes to do it, in the weapon's own frame, so a
     tracked controller can be scored against a real zone rather than a
     guessed one.
  3. WHEN in the motion the ammunition actually changes hands, so exactly one
     engine reload command can be issued at the right moment.

(1) is decided from the game's own fields; (2) and (3) are MEASURED from the
weapon's own reload animation, which is authored ground truth about where the
hand travels. Nothing here is invented.

THE DISCRIMINANT
----------------
Two authored signals, checked independently, decide the class:

  reloadStartAnim + reloadEndAnim present
      A start/loop/end reload: the engine plays an opening motion, repeats a
      feeding motion once per unit of ammunition, then a closing motion. Only
      per-round loaders are built this way. -> INCREMENTAL

  the support hand's excursion during the reload
      Measured in the weapon frame. A detachable magazine is fetched from and
      returned to a point BELOW the bore line (the magwell); a clip or charger
      is pressed in from ABOVE, at the receiver. The sign of the vertical
      excursion is what separates them, and it is a fact about the shipped
      animation rather than an opinion about the firearm.

Where the two agree, the class is PROVEN. Where only one is available it is
STRONG. Where the animation cannot be posed the class falls back to the
game-field inference alone and is marked WEAK -- and WEAK never becomes a
production constant (see vr_weapon_profile.h's confidence rules).

UNITS AND FRAME
---------------
Everything is METRES in the WEAPON FRAME (tag_weapon, +X forward, +Y left,
+Z up), matching tools/extract_geometry.py so the two tables compose.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
from extract_geometry import (Assets, merge_skeletons, pose_model_space,
                              basis_to_local)
import reload_classify as RCL

GAME_MAIN = CA.GAME_MAIN

UNITS_PER_M = 39.3701          # CoD unit is an inch

UNKNOWN, NA, WEAK, STRONG, PROVEN = (RCL.UNKNOWN, RCL.NA, RCL.WEAK,
                                      RCL.STRONG, RCL.PROVEN)


# Bones. The hands rig names both wrists explicitly; the flag byte confirms
# laterality where the names lie (`r wrist01` is flagged LEFT on this rig).
BONE_L_HAND = "bip01 l hand"
BONE_R_HAND = "bip01 r hand"
FRAME_BONE  = "tag_weapon"


def parse_weaponfile(text):
    parts = text.split(chr(92))
    f = {}
    for i in range(1, len(parts) - 1, 2):
        f[parts[i]] = parts[i + 1]
    return f


def vsub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def vlen(v):
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5


def value(v, conf, evidence, extra=None):
    d = {"value": v, "confidence": conf, "evidence": evidence}
    if extra:
        d.update(extra)
    return d



def hand_track(bones, index, anim, frames):
    """Left/right hand position per frame, expressed in the weapon frame.

    Returns (list of {frame, l, r}, note). Positions are metres. A frame the
    animation does not drive is skipped rather than guessed.
    """
    li = index.get(BONE_L_HAND)
    ri = index.get(BONE_R_HAND)
    fi = index.get(FRAME_BONE)
    if li is None or ri is None or fi is None:
        return [], "rig lacks %s / %s / %s" % (BONE_L_HAND, BONE_R_HAND, FRAME_BONE)

    out = []
    for fr in frames:
        posed = pose_model_space(bones, [(anim, fr)])
        if not posed or li not in posed or ri not in posed or fi not in posed:
            continue
        # resolve_model_space returns {index: (origin3, basis3x3)}.
        forigin, fbasis = posed[fi]
        # Express both hands in the weapon's own basis, so a reload measured
        # on a gun the animation also swings around does not read the swing.
        lp = basis_to_local(fbasis, vsub(posed[li][0], forigin))
        rp = basis_to_local(fbasis, vsub(posed[ri][0], forigin))
        out.append({
            "frame": fr,
            "l": [c / UNITS_PER_M for c in lp],
            "r": [c / UNITS_PER_M for c in rp],
        })
    return out, ""


def excursion(track, key):
    """Peak displacement of one hand from its frame-0 position, and where."""
    if not track:
        return 0.0, None, None
    base = track[0][key]
    far = max(track, key=lambda p: vlen(vsub(p[key], base)))
    return vlen(vsub(far[key], base)), far[key], far["frame"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=GAME_MAIN)
    ap.add_argument("--out",  default="docs/weapon_reload.json")
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)

    weapon_files = {}
    for p in fs.list("weapons/sp/"):
        weapon_files[os.path.basename(p)] = p

    out = []
    for name in sorted(weapon_files):
        raw = fs.read(weapon_files[name])
        if raw is None:
            continue
        fields = parse_weaponfile(raw.decode("latin-1"))
        wclass = fields.get("weaponClass", "")
        wtype  = fields.get("weaponType", "")

        rec = {
            "weapon": name,
            "weaponClass": wclass,
            "weaponType": wtype,
            "clipSize": fields.get("clipSize", ""),
            "boltAction": fields.get("boltAction", ""),
            "reloadAnim": fields.get("reloadAnim", ""),
            "reloadEmptyAnim": fields.get("reloadEmptyAnim", ""),
            "reloadStartAnim": fields.get("reloadStartAnim", ""),
            "reloadEndAnim": fields.get("reloadEndAnim", ""),
            "reloadTime": fields.get("reloadTime", ""),
            "reloadEmptyTime": fields.get("reloadEmptyTime", ""),
            "reloadAddTime": fields.get("reloadAddTime", ""),
        }

        # Prefer the START animation for a start/loop/end reload: the loop is
        # one round's worth of motion and understates the reach badly.
        anim_name = (fields.get("reloadStartAnim", "")
                     or fields.get("reloadEmptyAnim", "")
                     or fields.get("reloadAnim", ""))
        rec["measuredAnim"] = anim_name

        track, note = [], ""
        gun_parts, _  = assets.parts_for(fields.get("gunModel", ""))
        hand_parts, _ = assets.parts_for(fields.get("handModel", ""))
        anim = assets.anim(anim_name)
        if gun_parts is not None and hand_parts is not None and anim is not None:
            bones, index, anchor = merge_skeletons(hand_parts, gun_parts)
            if anchor is not None:
                n = anim.num_frames
                frames = sorted(set(list(range(0, n, max(1, n // 32))) + [n - 1]))
                track, note = hand_track(bones, index, anim, frames[:33])
        if not track and not note:
            note = "gun/hand model or reload animation unavailable"
        rec["trackFrames"] = len(track)
        rec["trackNote"] = note

        lreach, lfar, lframe = excursion(track, "l")
        rreach, rfar, rframe = excursion(track, "r")

        rclass, rconf, rev, hand, hconf, hev = RCL.classify(
            fields, bool(track), lreach, rreach, wclass, wtype, anim_name)
        rec["reloadClass"] = value(rclass, rconf, rev)
        rec["loadHand"]    = value(hand, hconf, hev)

        rec["supportReachM"] = value(round(lreach, 5), RCL.zone_conf(lreach),
                                     "peak support-hand excursion in %s" % anim_name)
        rec["triggerReachM"] = value(round(rreach, 5), RCL.zone_conf(rreach),
                                     "peak trigger-hand excursion in %s" % anim_name)

        key   = "r" if hand == RCL.HAND_TRIGGER else "l"
        reach = rreach if key == "r" else lreach
        far   = rfar   if key == "r" else lfar
        frame = rframe if key == "r" else lframe
        rec["loadReachM"] = value(round(reach, 5), RCL.zone_conf(reach),
                                  "excursion of the %s hand, which performs this reload"
                                  % ("trigger" if key == "r" else "support"))
        if track:
            rec["gripM"] = value([round(c, 5) for c in track[0][key]], RCL.STRONG,
                                 "loading hand at frame 0 of %s" % anim_name)
            rec["loadZoneM"] = value([round(c, 5) for c in far], RCL.zone_conf(reach),
                                     "loading-hand extremum at frame %s of %s"
                                     % (frame, anim_name))
            # Both hands' firing-pose positions, for the gesture machine's
            # "has the support hand left the weapon" test.
            rec["supportGripM"] = value([round(c, 5) for c in track[0]["l"]], RCL.STRONG,
                                        "support hand at frame 0 of %s" % anim_name)
            rec["triggerGripM"] = value([round(c, 5) for c in track[0]["r"]], RCL.STRONG,
                                        "trigger hand at frame 0 of %s" % anim_name)
        else:
            for k in ("gripM", "loadZoneM", "supportGripM", "triggerGripM"):
                rec[k] = value(None, RCL.UNKNOWN, note)

        mt, mconf, mev = RCL.magazine_type(fields, rclass, hand, reach)
        rec["magazineType"] = value(mt, mconf, mev)

        try:
            t  = float(fields.get("reloadEmptyTime") or fields.get("reloadTime") or 0)
            ta = float(fields.get("reloadAddTime") or 0)
            rec["ammoAddFrac"] = (value(round(ta / t, 4), RCL.PROVEN,
                                        "reloadAddTime / reloadEmptyTime from the weapon file")
                                  if t > 0 and ta > 0 else
                                  value(None, RCL.UNKNOWN, "weapon file gives no reloadAddTime"))
        except ValueError:
            rec["ammoAddFrac"] = value(None, RCL.UNKNOWN, "unparseable reload times")

        out.append(rec)

    with open(args.out, "w") as f:
        json.dump({"weapons": out}, f, indent=1)

    print("wrote %s (%d weapons)" % (args.out, len(out)))
    print()
    print("%-22s %-12s %-7s %-8s %-7s %-14s %-7s %s"
          % ("weapon", "class", "conf", "hand", "conf", "magazine", "conf", "reach"))
    print("-" * 108)
    for r in out:
        if r["reloadClass"]["value"] == RCL.RC_NONE:
            continue
        print("%-22s %-12s %-7s %-8s %-7s %-14s %-7s %.3f" % (
            r["weapon"], r["reloadClass"]["value"], r["reloadClass"]["confidence"],
            r["loadHand"]["value"], r["loadHand"]["confidence"],
            r["magazineType"]["value"], r["magazineType"]["confidence"],
            r["loadReachM"]["value"] or 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
