"""Measurement half of the reload extractor (see reload_classify.py for the
decision rules). Appended to extract_reload.py's preamble by hand once; kept
as its own file so the two halves stay legible.
"""


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
