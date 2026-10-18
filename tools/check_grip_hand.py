#!/usr/bin/env python
"""Which PALM does the authored idle pose place at the weapon's rear grip?

The hands rig's bone labels are proven unreliable (asset-L renders as the
player's RIGHT hand), and the profile extractor measured 'rearGripOffset'
from `bip01 r hand` by NAME. Before the runtime binds "gun follows the RIGHT
controller" to a palm, this settles -- from posed assets, no labels trusted
-- which rendered-side palm actually sits at the kGunModels rear-grip point.

Prints, per weapon: distance from each palm bone to the rear-grip world
point in the idle pose, and which surface (hence which RENDERED side, via
the proven surface->side mapping) that palm bone drives.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codasset as CA
from extract_geometry import Assets, merge_skeletons, pose_model_space

UNITS_PER_M = 39.3701


def parse_weaponfile(text):
    parts = text.split(chr(92))
    return {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}


def main():
    fs = CA.Pk3Fs(CA.GAME_MAIN)
    assets = Assets(fs)
    guns = {m["gunModel"].lower(): m
            for m in json.load(open("docs/gun_landmarks.json"))["models"]}

    for wname in ("thompson", "sten", "kar98k", "mp40", "m1garand", "bar"):
        fields = parse_weaponfile(fs.read("weapons/sp/" + wname).decode("latin-1"))
        rec = guns.get(fields["gunModel"].lower())
        if not rec or not rec["rearGripM"]["value"]:
            print("%-10s: no rear grip" % wname)
            continue
        hand_parts, _ = assets.parts_for(fields["handModel"])
        gun_model = assets.model(fields["gunModel"])
        gun_parts = CA.parse_xmodelparts(
            fs.read("xmodelparts/" + gun_model.parts_name), gun_model.parts_name)
        bones, index, anchor = merge_skeletons(hand_parts, gun_parts)
        body_i = index[rec["bodyBone"].lower()]
        li = index.get("bip01 l hand")
        ri = index.get("bip01 r hand")
        idle = assets.anim(fields.get("idleAnim", ""))
        posed = pose_model_space(bones, [(idle, 0)])
        bo, bb = posed[body_i]
        rg = [c * UNITS_PER_M for c in rec["rearGripM"]["value"]]
        world = [bo[k] + CA.mat_apply(bb, rg)[k] for k in range(3)]

        def dist_to(bi, pt):
            o, _ = posed[bi]
            return math.sqrt(sum((o[k] - pt[k]) ** 2 for k in range(3))) / UNITS_PER_M

        dl, dr = dist_to(li, world), dist_to(ri, world)

        # Label-free physics: the REAR hand is the one FARTHER from the
        # muzzle. muzzleOrigin comes from tag_flash (STRONG), tag_weapon
        # frame -- resolve it through the posed tag_weapon.
        geo = json.load(open("docs/weapon_geometry.json"))
        rows = geo.get("weapons", geo) if isinstance(geo, dict) else geo
        wrow = next(r for r in rows if r.get("weapon") == wname)
        mo = wrow.get("muzzleOrigin", {}).get("value")
        muz = ""
        if mo:
            ti = index.get("tag_weapon")
            if ti in posed:
                to, tb = posed[ti]
                mworld = [to[k] + CA.mat_apply(tb, list(mo))[k] for k in range(3)]
                ml, mr = dist_to(li, mworld), dist_to(ri, mworld)
                rear = "assetL" if ml > mr else "assetR"
                muz = ("  muzzle dist: assetL %.3f assetR %.3f -> REAR hand is %s"
                       % (ml, mr, rear))
        print("%-10s gripPt dist: assetL %.3f  assetR %.3f%s"
              % (wname, dl, dr, muz))


if __name__ == "__main__":
    sys.exit(main())
