#!/usr/bin/env python3
"""Extract the CoD1 single-player weapon inventory straight from the shipped
pk3 archives.

The pk3s are ordinary ZIPs; `weapons/sp/<name>` inside them is a flat
backslash-delimited key/value record that starts with the literal WEAPONFILE.
Later pak numbers override earlier ones (the engine's own load order), so the
last archive that carries a path wins here too.

Nothing in here touches an executable, a protected page, or the game process:
it is pure data-file reading, which is what makes the weapon inventory
provable offline instead of guessed.

Usage:
    python tools/extract_weapons.py [--json out.json] [--md out.md]
"""

import argparse
import collections
import glob
import json
import os
import zipfile

GAME_MAIN = r"H:\SteamLibrary\steamapps\common\Call of Duty\Main"

SEP = chr(92)  # backslash, spelled this way so no shell/heredoc can eat it


def load_sp_weapons(main_dir):
    """Return {name: (pak_basename, dict_of_fields)} for weapons/sp/*."""
    paks = sorted(glob.glob(os.path.join(main_dir, "*.pk3")))
    found = {}
    for pak in paks:
        try:
            zf = zipfile.ZipFile(pak)
        except Exception:
            continue
        with zf:
            for entry in zf.namelist():
                low = entry.lower()
                if not low.startswith("weapons/sp/") or low.endswith("/"):
                    continue
                name = low.rsplit("/", 1)[-1]
                found[name] = (pak, entry)
    out = {}
    for name, (pak, entry) in sorted(found.items()):
        with zipfile.ZipFile(pak) as zf:
            raw = zf.read(entry).decode("latin-1")
        fields = parse_weaponfile(raw)
        out[name] = (os.path.basename(pak), fields)
    return out


def parse_weaponfile(raw):
    parts = raw.split(SEP)
    if parts and parts[0].startswith("WEAPONFILE"):
        parts = parts[1:]
    fields = {}
    for i in range(0, len(parts) - 1, 2):
        key = parts[i].strip()
        if key:
            fields[key] = parts[i + 1]
    return fields


def key_census(weapons):
    census = collections.Counter()
    for _, fields in weapons.values():
        census.update(fields.keys())
    return census


# ---------------------------------------------------------------- VR class
#
# The mapping from the game's own weaponType/weaponClass/weaponSlot to the VR
# handling class. Everything here is READ from the weapon file; nothing is
# inferred from a model's geometry, because model geometry is exactly the part
# that needs hardware measurement.

def vr_class(name, f):
    """Classify from the weapon file's OWN fields only.

    Deliberately no name matching. An earlier version special-cased "30cal"
    into the mounted bucket on its name and was simply wrong: `30cal` is a
    carried weapon (weaponClass rifle, weaponSlot primary, twoHanded 1, and a
    real gunModel), while `30cal_prone` / `30cal_stand` are the mounted
    variants -- and the game already says so in the fields. Reading the data
    instead of the string keeps every row defensible.
    """
    wtype = f.get("weaponType", "").lower()
    wclass = f.get("weaponClass", "").lower()
    slot = f.get("weaponSlot", "").lower()

    if not f:
        return "unknown"
    if wtype == "grenade" or wclass == "grenade":
        return "grenade"
    # Turrets and "non-player" weapons carry no viewmodel and no hands at all;
    # the player is bolted to them by the engine.
    if wclass in ("turret", "non-player") or slot == "none":
        return "mounted"
    if wclass == "rocketlauncher":
        return "launcher"
    if wclass == "pistol" or slot == "pistol":
        return "pistol"
    if wclass == "smg":
        return "smg"
    if wclass == "mg":
        return "machinegun"
    if wclass in ("rifle", "spread"):
        # A "rifle" that cannot aim down sights is a carried prop, not a gun
        # (stalingrad_ammo is an ammo crate on the viewmodel_ammo model).
        return "rifle" if f.get("aimDownSight") == "1" else "prop"
    return "special"


# Fallback only. The game ships its own `twoHanded` flag on every weapon that
# has a viewmodel, and that field wins wherever it is present.
TWO_HAND_BY_CLASS = {
    "rifle": "YES",
    "smg": "YES",
    "machinegun": "YES",
    "launcher": "YES",
    "pistol": "OPTIONAL",
    "grenade": "NO",
    "mounted": "NO",
    "melee": "NO",
    "prop": "NO",
    "special": "UNKNOWN",
    "unknown": "UNKNOWN",
}


def two_hand_allowed(cls, f):
    """The game's own twoHanded flag, reconciled with the VR class.

    twoHanded==1 means "the model is held in both hands", which is exactly the
    physical question the VR support-hand solver asks. Where the game is
    silent (turrets have no such field) the class fallback answers.
    """
    flag = f.get("twoHanded")
    if flag == "1":
        # Mounted weapons never route through the hand solver even if the
        # underlying model is two-handed -- the engine owns the pose. Nor does
        # a carried prop: stalingrad_ammo really is held in two hands, but it
        # has no barrel to steer, so the rifle solver has nothing to solve.
        if cls == "mounted":
            return "NO (mounted)"
        if cls == "prop":
            return "NO (prop)"
        return "YES"
    if flag == "0":
        return "OPTIONAL" if cls == "pistol" else "NO"
    return TWO_HAND_BY_CLASS.get(cls, "UNKNOWN")


def summarise(name, pak, f):
    has_ads = bool(f.get("adsUpAnim") or f.get("adsDownAnim"))
    cls = vr_class(name, f)
    return collections.OrderedDict(
        name=name,
        pak=pak,
        weaponType=f.get("weaponType", ""),
        weaponClass=f.get("weaponClass", ""),
        weaponSlot=f.get("weaponSlot", ""),
        displayName=f.get("displayName", ""),
        gunModel=f.get("gunModel", ""),
        handModel=f.get("handModel", ""),
        worldModel=f.get("worldModel", ""),
        vrClass=cls,
        gameTwoHanded=f.get("twoHanded", ""),
        twoHandAllowed=two_hand_allowed(cls, f),
        carried=int(bool(f.get("gunModel"))),
        aimDownSight=f.get("aimDownSight", ""),
        hasAds=int(has_ads),
        adsUpAnim=f.get("adsUpAnim", ""),
        adsDownAnim=f.get("adsDownAnim", ""),
        adsZoomFov=f.get("adsZoomFov", ""),
        adsTransInTime=f.get("adsTransInTime", ""),
        adsTransOutTime=f.get("adsTransOutTime", ""),
        adsOverlayShader=f.get("adsOverlayShader", ""),
        boltAction=f.get("boltAction", ""),
        semiAuto=f.get("semiAuto", ""),
        # Stance offsets: the engine's OWN per-stance viewmodel translation,
        # in the same inches/Quake-axes frame the ADS animation uses.
        duckedOfs=[f.get("duckedOfsF", ""), f.get("duckedOfsR", ""), f.get("duckedOfsU", "")],
        proneOfs=[f.get("proneOfsF", ""), f.get("proneOfsR", ""), f.get("proneOfsU", "")],
        swayMaxAngle=f.get("swayMaxAngle", ""),
        hipIdleAmount=f.get("hipIdleAmount", ""),
        clipSize=f.get("clipSize", ""),
        fireTime=f.get("fireTime", ""),
        projectileSpeed=f.get("projectileSpeed", ""),
        fuseTime=f.get("fuseTime", ""),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=GAME_MAIN)
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--keys", action="store_true", help="print the key census")
    args = ap.parse_args()

    weapons = load_sp_weapons(args.main)
    print("SP weapon files found: %d" % len(weapons))

    if args.keys:
        for key, n in sorted(key_census(weapons).items()):
            print("  %-28s %d" % (key, n))

    rows = [summarise(n, pak, f) for n, (pak, f) in weapons.items()]

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=2)
        print("wrote " + args.json)

    if args.md:
        cols = ["name", "vrClass", "weaponType", "weaponClass", "weaponSlot",
                "gunModel", "handModel", "gameTwoHanded", "aimDownSight",
                "twoHandAllowed"]
        with open(args.md, "w", encoding="utf-8") as fh:
            fh.write("| " + " | ".join(cols) + " |\n")
            fh.write("|" + "|".join(["---"] * len(cols)) + "|\n")
            for r in rows:
                fh.write("| " + " | ".join(str(r[c]) for c in cols) + " |\n")
        print("wrote " + args.md)

    byclass = collections.Counter(r["vrClass"] for r in rows)
    print("by VR class: " + ", ".join("%s=%d" % kv for kv in sorted(byclass.items())))


if __name__ == "__main__":
    main()
