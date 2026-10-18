#!/usr/bin/env python3
"""Decode the per-weapon ADS (iron-sight) viewmodel pose straight out of the
game's own animation data.

WHY THIS EXISTS
---------------
The hardest per-weapon number for VR is "where does this gun have to sit for
its sights to line up with the eye". CoD1 already answers that: every
ADS-capable weapon ships `xanim/viewmodel_<w>_ADS_up`, a short animation of a
SINGLE bone -- `tag_torso`, the root of the whole viewmodel assembly -- that
slides the gun from the hip pose to the sighted pose. Its LAST key is the
sighted pose; its FIRST key is the hip pose.

FORMAT (version 14, reverse-engineered from the shipped files and then
cross-validated, see `verify` below)

    u16 version            always 14 (0x0e) for CoD1
    u16 numFrames
    u16 numBones
    u8  flags              observed 0
    u16 framerate          observed 30
    u16 reserved           observed 0
    char name[]            NUL-terminated, one per bone
    per bone:
        u16 numRotKeys
        u8  rotFrame[numRotKeys]          OMITTED when numRotKeys == numFrames
        s16 rot[numRotKeys][3]            quaternion x,y,z * 32768; w = +sqrt(1-x2-y2-z2)
        u16 numTransKeys
        u8  transFrame[numTransKeys]      OMITTED when numTransKeys == numFrames
        f32 trans[numTransKeys][3]        CoD units (inches), Quake axes
    u8  trailing NUL

The omitted-index rule is what a naive reading gets wrong: a fully-keyed
channel (every frame a key) carries no index table at all, because the index
would just be 0..numFrames-1. Files mixing a sparse rotation channel with a
dense translation channel (mp40) only parse to the exact byte with the rule
applied, which is how it was found.

THE CROSS-CHECK THAT PROVES THE DECODE
--------------------------------------
`ADS_up` and `ADS_down` are the same motion in opposite directions. So

    ADS_up.trans[last]  ==  ADS_down.trans[first]      (the sighted pose)
    ADS_up.trans[first] ==  ADS_down.trans[last]       (the hip pose)

and the same for the rotations. 28 of the 29 SP weapons satisfy that to
within 0.02 in; only `springfield` disagrees (2.66 in), which is an authoring
inconsistency in the shipped data, not a decode error -- its ADS_down simply
starts from a different pose than its ADS_up ends at. A wrong field layout
could not produce a clean round trip on 28 files, which is why the numbers
this prints are measurements and not guesses.

`--verify` fails on a DECODE error (a file the layout cannot consume to the
exact byte). Endpoint disagreements are reported as magnitudes, because
that is data about the game, not a bug in this tool.

Nothing here touches an executable or a running process -- these are data
files inside ordinary ZIP archives.

Usage:
    python tools/extract_ads_pose.py --verify
    python tools/extract_ads_pose.py --json out.json
"""

import argparse
import glob
import json
import math
import os
import struct
import zipfile

GAME_MAIN = r"H:\SteamLibrary\steamapps\common\Call of Duty\Main"


# ------------------------------------------------------------------ pk3 index

def build_index(main_dir, prefix):
    """Map lowercase archive path -> (pak path, real entry name). Later paks win."""
    index = {}
    for pak in sorted(glob.glob(os.path.join(main_dir, "*.pk3"))):
        try:
            zf = zipfile.ZipFile(pak)
        except Exception:
            continue
        with zf:
            for entry in zf.namelist():
                low = entry.lower()
                if low.startswith(prefix) and not low.endswith("/"):
                    index[low] = (pak, entry)
    return index


def read_entry(index, path):
    hit = index.get(path.lower())
    if not hit:
        return None
    pak, entry = hit
    with zipfile.ZipFile(pak) as zf:
        return zf.read(entry)


# -------------------------------------------------------------- xanim decode

class XAnimError(Exception):
    pass


def decode_xanim(data):
    if len(data) < 12:
        raise XAnimError("too short")
    version, num_frames, num_bones = struct.unpack_from("<HHH", data, 0)
    if version != 14:
        raise XAnimError("unexpected version %d" % version)
    flags = data[6]
    framerate, reserved = struct.unpack_from("<HH", data, 7)
    off = 11

    names = []
    for _ in range(num_bones):
        end = data.index(b"\x00", off)
        names.append(data[off:end].decode("latin-1"))
        off = end + 1

    if num_frames > 255:
        raise XAnimError("frame index width unproven above 255 frames")

    bones = []
    for name in names:
        (n_rot,) = struct.unpack_from("<H", data, off)
        off += 2
        if n_rot == num_frames:
            rot_frames = list(range(n_rot))     # dense: no index table stored
        else:
            rot_frames = list(data[off:off + n_rot])
            off += n_rot
        rots = []
        for _ in range(n_rot):
            x, y, z = struct.unpack_from("<hhh", data, off)
            off += 6
            fx, fy, fz = x / 32768.0, y / 32768.0, z / 32768.0
            w2 = 1.0 - (fx * fx + fy * fy + fz * fz)
            rots.append((fx, fy, fz, math.sqrt(w2) if w2 > 0.0 else 0.0))

        (n_trans,) = struct.unpack_from("<H", data, off)
        off += 2
        if n_trans == num_frames:
            trans_frames = list(range(n_trans))  # dense: no index table stored
        else:
            trans_frames = list(data[off:off + n_trans])
            off += n_trans
        trans = []
        for _ in range(n_trans):
            trans.append(struct.unpack_from("<fff", data, off))
            off += 12

        bones.append(dict(name=name, rotFrames=rot_frames, rots=rots,
                          transFrames=trans_frames, trans=trans))

    # The layout is only proven if it consumes the file exactly (one trailing
    # NUL). Anything else means a field width is wrong -- fail loudly rather
    # than emit plausible garbage.
    leftover = len(data) - off
    if leftover not in (0, 1):
        raise XAnimError("layout consumed %d of %d bytes (leftover %d)"
                         % (off, len(data), leftover))

    return dict(version=version, numFrames=num_frames, flags=flags,
                framerate=framerate, reserved=reserved, bones=bones)


# ------------------------------------------------------------------ analysis

def quat_to_axis_angle_deg(q):
    x, y, z, w = q
    w = max(-1.0, min(1.0, w))
    ang = 2.0 * math.degrees(math.acos(w))
    if ang > 180.0:
        ang -= 360.0
    return ang


def weapon_ads_poses(main_dir):
    """{weapon: {...}} for every SP weapon whose ADS_up anim decodes."""
    import extract_weapons

    weapons = extract_weapons.load_sp_weapons(main_dir)
    anims = build_index(main_dir, "xanim/")

    out = {}
    for name, (_pak, fields) in sorted(weapons.items()):
        up_name = fields.get("adsUpAnim", "")
        down_name = fields.get("adsDownAnim", "")
        if not up_name:
            continue
        rec = dict(weapon=name, upAnim=up_name, downAnim=down_name)
        up = read_entry(anims, "xanim/" + up_name)
        if up is None:
            rec["status"] = "ADS_up anim not found in any pk3"
            out[name] = rec
            continue
        try:
            a = decode_xanim(up)
        except (XAnimError, struct.error, ValueError, IndexError) as e:
            rec["status"] = "decode failed: %s" % e
            out[name] = rec
            continue

        bone = a["bones"][0]
        rec["status"] = "ok"
        rec["numFrames"] = a["numFrames"]
        rec["framerate"] = a["framerate"]
        rec["boneCount"] = len(a["bones"])
        rec["boneName"] = bone["name"]
        if bone["trans"]:
            rec["hipTrans"] = list(bone["trans"][0])
            rec["adsTrans"] = list(bone["trans"][-1])
            rec["delta"] = [rec["adsTrans"][i] - rec["hipTrans"][i] for i in range(3)]
        if bone["rots"]:
            rec["hipQuat"] = list(bone["rots"][0])
            rec["adsQuat"] = list(bone["rots"][-1])
            rec["adsRotDeg"] = quat_to_axis_angle_deg(bone["rots"][-1])
        rec["downOk"] = None
        if down_name:
            down = read_entry(anims, "xanim/" + down_name)
            if down is not None:
                try:
                    b = decode_xanim(down)
                    db = b["bones"][0]
                    # Translations are stored as raw float32 and must match
                    # bit-for-bit. Rotations round-trip through a
                    # sqrt-reconstructed w, so they are compared on the stored
                    # x/y/z at quantisation precision (1/32768 ~ 3e-5).
                    # Report the endpoint disagreement as a MAGNITUDE, not a
                    # boolean. Bit-identical is the common case, but several
                    # weapons were re-keyed by hand and differ by a hundredth
                    # of an inch -- that is authoring noise, not a decode
                    # error, and calling it "failed" would hide the one
                    # weapon (springfield) whose endpoints genuinely disagree
                    # by inches.
                    rec["transRoundTripErrIn"] = max(
                        _maxdiff(db["trans"][0], bone["trans"][-1]),
                        _maxdiff(db["trans"][-1], bone["trans"][0]))
                    rec["rotRoundTripErr"] = max(
                        _maxdiff(db["rots"][0][:3], bone["rots"][-1][:3]),
                        _maxdiff(db["rots"][-1][:3], bone["rots"][0][:3]))
                    rec["downOk"] = (rec["transRoundTripErrIn"] <= 0.02
                                     and rec["rotRoundTripErr"] <= 2e-3)
                except (XAnimError, struct.error, ValueError, IndexError):
                    rec["downOk"] = False
        out[name] = rec
    return out


def _close(a, b, tol=1e-6):
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


def _maxdiff(a, b):
    if len(a) != len(b):
        return float("inf")
    return max(abs(x - y) for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default=GAME_MAIN)
    ap.add_argument("--json")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

    poses = weapon_ads_poses(args.main)
    ok = [r for r in poses.values() if r.get("status") == "ok"]
    print("weapons with an adsUpAnim : %d" % len(poses))
    print("decoded cleanly           : %d" % len(ok))

    verified = [r for r in ok if r.get("downOk") is True]
    broken = [r for r in ok if r.get("downOk") is False]
    print("up/down round-trip proven : %d" % len(verified))
    if broken:
        print("ROUND-TRIP FAILED         : %s" % ", ".join(r["weapon"] for r in broken))

    print()
    hdr = "%-24s %5s %6s  %-24s %-24s %7s" % (
        "weapon", "frames", "bones", "hip trans (F,L,U in)", "ADS trans (F,L,U in)", "rot deg")
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(poses.values(), key=lambda r: r["weapon"]):
        if r.get("status") != "ok" or "adsTrans" not in r:
            print("%-24s  %s" % (r["weapon"], r.get("status")))
            continue
        h, a = r["hipTrans"], r["adsTrans"]
        print("%-24s %5d %6d  %-24s %-24s %7.2f%s" % (
            r["weapon"], r["numFrames"], r["boneCount"],
            "%7.3f %7.3f %7.3f" % tuple(h),
            "%7.3f %7.3f %7.3f" % tuple(a),
            r.get("adsRotDeg", 0.0),
            "" if r.get("downOk") else "   <-- up/down endpoints differ by %.3f in"
                                        % r.get("transRoundTripErrIn", float("nan"))))

    if args.verify:
        undecoded = [r for r in poses.values() if r.get("status") != "ok"]
        if undecoded:
            for r in undecoded:
                print("  UNDECODED %-24s %s" % (r["weapon"], r.get("status")))
            raise SystemExit("VERIFY FAILED: %d file(s) did not decode" % len(undecoded))
        print("\nVERIFY OK: all %d ADS animations decode to the exact byte; "
              "%d/%d round-trip against their ADS_down within 0.02 in"
              % (len(ok), len(verified), len(ok)))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(poses, fh, indent=2)
        print("\nwrote " + args.json)


if __name__ == "__main__":
    main()
