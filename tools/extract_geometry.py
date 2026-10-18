#!/usr/bin/env python3
"""Derive per-weapon VR calibration geometry from the shipped CoD1 assets.

WHAT THIS REPLACES
------------------
Six per-weapon numbers were AUTO -- meaning "measure it on the headset":
rearGripOffset, rearGripRotation, supportAlongAxis, supportRadius,
sightForward, and the muzzle/barrel axis. The game already knows most of them,
because a 2003 first-person shooter has to: the viewmodel is a real skeleton
with a real weapon grafted onto it, and the idle animation poses both hands ON
the gun. That pose is data, not opinion.

WHAT IT DELIBERATELY DOES NOT DO
--------------------------------
It does not convert a flat-screen presentation into a physical grip without
saying so. The viewmodel pose says where the artist put the hands relative to
the weapon -- which is exactly the rear/support grip question -- but it says
nothing about where a headset's tracked controller will be relative to the
player's real hand. So every value carries a confidence, evidence is recorded
per value, and nothing is promoted to PROVEN on a single unchecked inference.

CONFIDENCE
----------
PROVEN   two independent lines of evidence agree (e.g. the muzzle direction
         from tag_flash's POSITION and from tag_flash's own ROTATION basis,
         within tolerance)
STRONG   one authored, named, semantic source -- a real tag or a real
         animation channel -- consistent across the frames we sample
WEAK     inferred from geometry with no named tag backing it
UNKNOWN  no evidence in the assets

Usage:
    python tools/extract_geometry.py --json ../docs/weapon_geometry.json
                                     --md   ../docs/WEAPON-GEOMETRY-INVENTORY.md
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import codasset as CA
import extract_weapons as EW

PROVEN, STRONG, WEAK, UNKNOWN = "PROVEN", "STRONG", "WEAK", "UNKNOWN"
# A class that has no barrel cannot have a barrel axis. Emitting one anyway --
# the grenades all carry a vestigial tag_flash -- would be exactly the kind of
# confident nonsense this pass exists to avoid.
NOT_APPLICABLE = "N/A"
BARRELLESS_CLASSES = ("grenade", "prop", "mounted", "melee", "unknown")

# Semantic tag names we specifically look for. The brief is right that naming
# is not consistent, so this is a search list, not an expectation.
SEMANTIC_TAGS = [
    "tag_weapon", "tag_flash", "tag_barrel", "tag_muzzle", "tag_clip",
    "tag_magazine", "tag_brass", "tag_sling", "tag_torso", "tag_view",
    "tag_hand", "tag_left", "tag_right", "tag_knife", "tag_bipod",
]

# Weapon-file fields whose animations keep BOTH hands on the weapon. These are
# the only ones the steady grip may be measured from.
# lastShotAnim is deliberately NOT here: on every bolt-action and on the
# self-loaders it runs the bolt or locks the slide, so the trigger hand comes
# off the grip. It measured as a 6 in "wander" on the kar98k until it was
# moved to the excluded list, where it belongs.
STEADY_ANIM_FIELDS = ("idleAnim", "emptyIdleAnim", "fireAnim", "adsFireAnim")
# ...and the ones that must never be used for it, because the hands leave the
# weapon in all of them. Measured, not assumed: the report records how far the
# support hand actually travels in these, which is the evidence that excluding
# them matters.
TRANSIENT_ANIM_FIELDS = ("reloadAnim", "reloadEmptyAnim", "meleeAnim",
                         "dropAnim", "raiseAnim", "rechamberAnim",
                         "adsRechamberAnim", "altDropAnim", "altRaiseAnim",
                         "lastShotAnim", "adsLastShotAnim")

# Two independent muzzle-direction estimates must agree to this to be PROVEN.
MUZZLE_AGREE_DEG = 5.0
# A hand that moves more than this away from its authored idle position during
# the weapon's own firing animation is not holding the weapon there.
SUPPORT_STABLE_IN = 3.0
GRIP_STABLE_IN = 3.0
# No part of a held weapon is ever this far from the eye. Beyond it, the model
# has been thrown or launched and the frame is not a grip pose at all.
MAX_PLAUSIBLE_REACH_IN = 60.0


def vlen(v):
    return CA.vec_len(v)


def basis_to_local(basis, world_vec):
    """World vector -> that basis's local frame. Rows of `basis` are its axes."""
    return (CA.vec_dot(world_vec, (basis[0], basis[1], basis[2])),
            CA.vec_dot(world_vec, (basis[3], basis[4], basis[5])),
            CA.vec_dot(world_vec, (basis[6], basis[7], basis[8])))


def mat_to_quat(m):
    """Row-major 3x3 -> (x, y, z, w)."""
    tr = m[0] + m[4] + m[8]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        return ((m[5] - m[7]) / s, (m[6] - m[2]) / s, (m[1] - m[3]) / s, 0.25 * s)
    if m[0] > m[4] and m[0] > m[8]:
        s = math.sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0
        return (0.25 * s, (m[3] + m[1]) / s, (m[6] + m[2]) / s, (m[5] - m[7]) / s)
    if m[4] > m[8]:
        s = math.sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0
        return ((m[3] + m[1]) / s, 0.25 * s, (m[7] + m[5]) / s, (m[6] - m[2]) / s)
    s = math.sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0
    return ((m[6] + m[2]) / s, (m[7] + m[5]) / s, 0.25 * s, (m[1] - m[3]) / s)


def mat_transpose(m):
    return (m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8])


# ------------------------------------------------------------ skeleton graft

def merge_skeletons(hands, gun):
    """Hands rig with the gun rig grafted onto its tag_weapon.

    This is how the engine assembles a viewmodel: one hands model owns the
    arms and a `tag_weapon` bone, and the weapon model -- whose own root IS
    `tag_weapon` -- hangs off it. Merging them is what makes a single
    model-space answer possible for "where is the left hand on this barrel".
    """
    bones = []
    index = {}

    def add(src, parent, name=None):
        b = CA.Bone()
        b.index = len(bones)
        b.name = name or src.name
        b.parent = parent
        b.trans = src.trans
        b.quat = src.quat
        b.mins = src.mins
        b.maxs = src.maxs
        b.flag = src.flag
        bones.append(b)
        index[b.name.lower()] = b.index
        return b.index

    for b in hands.bones:
        add(b, b.parent)
    anchor = index.get("tag_weapon")
    if gun is None or anchor is None:
        return bones, index, anchor

    remap = {}
    for b in gun.bones:
        if b.name.lower() == "tag_weapon":
            remap[b.index] = anchor
            continue
        remap[b.index] = add(b, -1)
    for b in gun.bones:
        if b.name.lower() == "tag_weapon":
            continue
        p = remap.get(b.parent, anchor) if b.parent >= 0 else anchor
        bones[remap[b.index]].parent = p
    return bones, index, anchor


def primary_mesh_bone(gun_parts):
    """The gun rig bone that carries the weapon body.

    Chosen as the largest per-bone AABB volume, which is unambiguous on every
    shipped weapon rig: the receiver/stock bone dwarfs the bolt, clip and
    sight bones by an order of magnitude. Used for two things -- as an
    independent estimate of the bore direction via its own AABB, and as the
    fallback answer frame for the rigs whose animation never drives
    tag_weapon (the panzerfaust and m1carbine root at `tag_origin` instead).
    """
    def depth(b):
        d, cur, guard = 0, b.parent, 0
        while cur >= 0 and guard < len(gun_parts.bones):
            cur = gun_parts.bones[cur].parent
            d += 1
            guard += 1
        return d

    geo = [b for b in gun_parts.bones if b.has_geometry]
    if not geo:
        return None
    # Shallowest first, then largest. Volume alone picks the panzerfaust's
    # ROCKET over its tube -- and the rocket leaves the weapon when fired, so
    # every offset measured against it would be measured against a part that
    # is not there a moment later.
    shallowest = min(depth(b) for b in geo)
    geo = [b for b in geo if depth(b) == shallowest]

    def vol(b):
        e = b.extent
        return max(e[0], 1e-3) * max(e[1], 1e-3) * max(e[2], 1e-3)

    return max(geo, key=vol)


def aabb_long_axis(bone):
    """The bone-local unit vector along its AABB's longest side, and its length."""
    e = bone.extent
    i = max(range(3), key=lambda k: e[k])
    v = [0.0, 0.0, 0.0]
    v[i] = 1.0
    return tuple(v), e[i], i


def pose_model_space(bones, anims_frames):
    """Resolve model space with a list of (XAnim, frame) layered in order."""
    override = {}
    name_to_i = {}
    for i, b in enumerate(bones):
        name_to_i.setdefault(b.name.lower(), i)
    for anim, frame in anims_frames:
        if anim is None:
            continue
        for b in anim.bones:
            i = name_to_i.get(b.name.lower())
            if i is None:
                continue
            t = b.trans_at(frame)
            q = b.rot_at(frame)
            if t is None and q is None:
                continue
            ot, oq = override.get(i, (bones[i].trans, bones[i].quat))
            override[i] = (t if t is not None else ot, q if q is not None else oq)
    return CA.resolve_model_space(bones, override)


# ------------------------------------------------------------------- loading

class Assets(object):
    def __init__(self, fs):
        self.fs = fs
        self._parts = {}
        self._models = {}
        self._anims = {}

    def model(self, name):
        if not name:
            return None
        key = name.lower()
        if key not in self._models:
            raw = self.fs.read("xmodel/" + name)
            self._models[key] = CA.parse_xmodel(raw, name) if raw else None
        return self._models[key]

    def parts_for(self, model_name):
        m = self.model(model_name)
        if m is None or not m.parts_name:
            return None, None
        key = m.parts_name.lower()
        if key not in self._parts:
            raw = self.fs.read("xmodelparts/" + m.parts_name)
            self._parts[key] = CA.parse_xmodelparts(raw, m.parts_name) if raw else None
        return self._parts[key], m.parts_name

    def anim(self, name):
        if not name:
            return None
        key = name.lower()
        if key not in self._anims:
            raw = self.fs.read("xanim/" + name)
            try:
                self._anims[key] = CA.parse_xanim(raw, name) if raw else None
            except CA.XAnimError:
                self._anims[key] = None
        return self._anims[key]


# ---------------------------------------------------------------- extraction

def steady_frames(anim, cap=16):
    if anim is None or anim.num_frames <= 0:
        return []
    n = anim.num_frames
    if n <= cap:
        return list(range(n))
    step = max(1, n // cap)
    return list(range(0, n, step))[:cap]


def value(v, conf, evidence, extra=None):
    d = {"value": v, "confidence": conf, "evidence": evidence}
    if extra:
        d.update(extra)
    return d


def extract_weapon(name, fields, assets, vrclass):
    """Everything derivable for one weapon. Values default to UNKNOWN."""
    rec = {
        "weapon": name,
        "vrClass": vrclass,
        "gunModel": fields.get("gunModel", ""),
        "handModel": fields.get("handModel", ""),
        "idleAnim": fields.get("idleAnim", ""),
        "adsUpAnim": fields.get("adsUpAnim", ""),
        "gameTwoHanded": fields.get("twoHanded", ""),
        "aimDownSight": fields.get("aimDownSight", ""),
        "status": "ok",
        "tags": {},
        "muzzleOrigin": value(None, UNKNOWN, "no evidence"),
        "barrelForward": value(None, UNKNOWN, "no evidence"),
        "rearGripOffset": value(None, UNKNOWN, "no evidence"),
        "rearGripRotation": value(None, UNKNOWN, "no evidence"),
        "supportAlongAxis": value(None, UNKNOWN, "no evidence"),
        "supportRadius": value(None, UNKNOWN, "no evidence"),
        "supportOffset": value(None, UNKNOWN, "no evidence"),
        "sightOffset": value(None, UNKNOWN, "no evidence"),
        "sightForward": value(None, UNKNOWN, "no evidence"),
        "physicalSightLine": UNKNOWN,
    }

    gun_parts, gun_pname = assets.parts_for(fields.get("gunModel", ""))
    hand_parts, hand_pname = assets.parts_for(fields.get("handModel", ""))
    rec["gunParts"] = gun_pname
    rec["handParts"] = hand_pname
    if gun_parts is None:
        rec["status"] = "no gun model"
        return rec
    if hand_parts is None:
        rec["status"] = "no hand model"

    rec["gunBones"] = [
        {"index": b.index, "name": b.name, "parent": b.parent,
         "parentName": (gun_parts.bones[b.parent].name if 0 <= b.parent < len(gun_parts.bones) else None),
         "trans": [round(v, 5) for v in b.trans],
         "quat": [round(v, 6) for v in b.quat],
         "mins": [round(v, 4) for v in b.mins],
         "maxs": [round(v, 4) for v in b.maxs],
         "hasGeometry": b.has_geometry, "flag": b.flag}
        for b in gun_parts.bones
    ]
    present = {}
    for b in gun_parts.bones:
        low = b.name.lower()
        for t in SEMANTIC_TAGS:
            if low == t or low.startswith(t + "_"):
                present.setdefault(t, []).append(b.name)
    if hand_parts is not None:
        for b in hand_parts.bones:
            low = b.name.lower()
            for t in SEMANTIC_TAGS:
                if low == t or low.startswith(t + "_"):
                    present.setdefault(t, []).append(b.name)
    rec["tags"] = present
    rec["gunTagNames"] = [b.name for b in gun_parts.bones if b.name.lower().startswith("tag_")]

    if hand_parts is None:
        return rec

    bones, index, anchor = merge_skeletons(hand_parts, gun_parts)
    rec["mergedBoneCount"] = len(bones)
    if anchor is None:
        rec["status"] = "hand rig has no tag_weapon"
        return rec

    idle = assets.anim(fields.get("idleAnim", ""))
    if idle is None:
        rec["status"] = "idle anim missing or undecodable"
        return rec
    rec["idleFrames"] = idle.num_frames

    # The steady grip is measured across every animation in which both hands
    # stay on the weapon, not just idle -- idle is often a single static frame,
    # which would make "wander" trivially zero and prove nothing.
    # An animation only counts if it actually drives THIS weapon's rig. The
    # shipped weapon files are not careful about this: sten_engineer's
    # emptyIdleAnim is `viewmodel_mk1_empty` -- the MK1 grenade -- and the
    # panzerfaust's is the stielhandgranate's. Both are copy-paste errors in
    # the game data, and both quietly wreck a grip measurement, so the filter
    # is a rig-membership test rather than a name test.
    gun_bone_names = set(b.name.lower() for b in gun_parts.bones
                         if b.has_geometry)

    def drives_this_weapon(a):
        if not gun_bone_names:
            return True
        return any(b.name.lower() in gun_bone_names for b in a.bones)

    steady = []
    seen_anims = set()
    rejected = []
    for fld in STEADY_ANIM_FIELDS:
        nm = fields.get(fld, "")
        if not nm or nm.lower() in seen_anims:
            continue
        a = assets.anim(nm)
        if a is None:
            continue
        seen_anims.add(nm.lower())
        if not drives_this_weapon(a):
            rejected.append("%s (%s: drives no bone of this gun rig)" % (nm, fld))
            continue
        steady.append(a)
    if idle not in steady and drives_this_weapon(idle):
        steady.insert(0, idle)
    if not steady:
        steady = [idle]
    rec["steadyAnims"] = [a.name for a in steady]
    if rejected:
        rec["rejectedAnims"] = rejected

    # Idle poses define the grip; firing poses only measure how far it moves.
    # Keeping them apart matters: a thompson's trigger hand travels 2.5 in
    # during its own fire animation, which is recoil authoring, not evidence
    # that the authored grip is unknown.
    idle_names = set()
    for fld in ("idleAnim", "emptyIdleAnim"):
        nm = fields.get(fld, "")
        if nm:
            idle_names.add(nm.lower())

    mesh = primary_mesh_bone(gun_parts)
    probe_i = index.get(mesh.name.lower()) if mesh is not None else None
    dropped = [0]

    def gather(anim_list):
        out = []
        for a in anim_list:
            for f in steady_frames(a):
                p = pose_model_space(bones, [(a, f)])
                # A pose in which the weapon has left the hands is not a pose
                # OF this weapon. Grenade and panzerfaust "fire" animations
                # throw the model away, and one reaches 71000 in from the eye;
                # averaging that in produced a grip offset of the same order.
                if probe_i is not None and probe_i in p:
                    if vlen(p[probe_i][0]) > MAX_PLAUSIBLE_REACH_IN:
                        dropped[0] += 1
                        continue
                out.append(p)
        return out

    idle_anims = [a for a in steady if a.name.lower() in idle_names] or [steady[0]]
    fire_anims = [a for a in steady if a.name.lower() not in idle_names]
    idle_poses = gather(idle_anims)
    fire_poses = gather(fire_anims)
    if not idle_poses:
        idle_poses = [pose_model_space(bones, [(idle, 0)])]
    poses = idle_poses + fire_poses
    rec["idlePoseCount"] = len(idle_poses)
    rec["firePoseCount"] = len(fire_poses)
    if dropped[0]:
        rec["posesDroppedOutOfReach"] = dropped[0]

    def pos(pose, bone_name):
        i = index.get(bone_name.lower())
        if i is None or i not in pose:
            return None
        return pose[i][0]

    def basis(pose, bone_name):
        i = index.get(bone_name.lower())
        if i is None or i not in pose:
            return None
        return pose[i][1]

    p0 = poses[0]

    # `tag_weapon` is the frame to answer in: it is the engine's own weapon
    # attach point, so it is what the VR renderer will actually position the
    # gun by, and it sits at the grip rather than wherever a mesh bone's pivot
    # happens to be. Expressing these offsets in the weapon BODY bone's frame
    # instead was tried and is wrong -- that pivot is arbitrary per weapon and
    # put the enfield's support hand 40 in from its own origin.
    # ...but only when the animation actually DRIVES tag_weapon. The
    # panzerfaust's idle never touches it, so there tag_weapon is still sitting
    # at the viewmodel root and "relative to tag_weapon" would silently mean
    # "relative to the eye" -- which is how that weapon ended up claiming a
    # grip 15 in below the muzzle. When it is undriven, fall back to the
    # weapon body bone, which the animation does drive.
    driven = set()
    for a in steady:
        for b in a.bones:
            if b.trans or b.rots:
                driven.add(b.name.lower())
    rec["tagWeaponAnimated"] = "tag_weapon" in driven

    frame_name = None
    if "tag_weapon" in driven and pos(p0, "tag_weapon") is not None:
        frame_name = "tag_weapon"
        rec["frameSource"] = "tag_weapon (animated)"
    elif mesh is not None and pos(p0, mesh.name) is not None:
        frame_name = mesh.name
        rec["frameSource"] = "weapon body bone (tag_weapon is not animated)"
    elif pos(p0, "tag_weapon") is not None:
        frame_name = "tag_weapon"
        rec["frameSource"] = "tag_weapon (bind pose only)"
    if frame_name is None:
        rec["status"] = "no resolvable weapon frame bone"
        return rec
    rec["weaponFrameBone"] = frame_name
    rec["weaponBodyBone"] = mesh.name if mesh is not None else None

    def offsets(bone_name, pose_list):
        """That bone in weapon-frame coordinates, once per sampled pose.

        The weapon frame is re-resolved in EVERY pose. Using frame 0's frame
        throughout was wrong and not subtly so: a fire animation recoils
        tag_weapon about six inches and back, and holding the frame fixed
        booked all of that as the HAND moving, turning a rock-steady grip into
        a 6 in wander.
        """
        out = []
        for p in pose_list:
            o = pos(p, frame_name)
            b = basis(p, frame_name)
            v = pos(p, bone_name)
            if o is None or v is None:
                continue
            out.append(basis_to_local(b, CA.vec_sub(v, o)))
        return out

    # Everything below is expressed relative to `frame_name`, which is only
    # meaningful if that frame is actually rigid with the visible gun. Measure
    # it rather than assume it: the weapon body bone's offset from the frame
    # must not move across the sampled poses.
    # Probe the weapon BODY specifically. Probing "whatever geometry bone is
    # not the frame" picks up bolts and rockets, which are supposed to move.
    if mesh is not None and mesh.name.lower() == frame_name.lower():
        rec["frameRigidityIn"] = 0.0
        rec["frameRigidityProbe"] = "frame is the weapon body bone"
    elif mesh is not None:
        rigid = offsets(mesh.name, poses)
        if len(rigid) > 1:
            m = tuple(sum(l[i] for l in rigid) / len(rigid) for i in range(3))
            rec["frameRigidityIn"] = round(
                max(vlen(CA.vec_sub(l, m)) for l in rigid), 4)
            rec["frameRigidityProbe"] = mesh.name
    w_org = pos(p0, frame_name)
    w_bas = basis(p0, frame_name)

    barrelless = vrclass in BARRELLESS_CLASSES

    # ------------------------------------------------- Phase 2: muzzle/barrel
    muzzle_tag = None
    for cand in ("tag_flash", "tag_muzzle", "tag_barrel"):
        b = gun_parts.by_name(cand)
        if b is not None and pos(p0, cand) is not None:
            muzzle_tag = cand
            break
    if muzzle_tag is None:
        for b in gun_parts.bones:
            low = b.name.lower()
            if low.startswith("tag_flash") or low.startswith("tag_muzzle"):
                if pos(p0, low) is not None:
                    muzzle_tag = low
                    break

    if barrelless:
        why = "vrClass %s has no barrel; the rig's tag_flash is vestigial" % vrclass
        rec["muzzleOrigin"] = value(None, NOT_APPLICABLE, why)
        rec["barrelForward"] = value(None, NOT_APPLICABLE, why)
    elif muzzle_tag and pos(p0, muzzle_tag) is not None:
        m_org = pos(p0, muzzle_tag)
        m_bas = basis(p0, muzzle_tag)
        delta = CA.vec_sub(m_org, w_org)
        local_org = basis_to_local(w_bas, delta)
        if vlen(delta) < 0.5:
            rec["muzzleOrigin"] = value(
                None, UNKNOWN,
                "%s resolves onto the weapon frame origin (%.3f in): degenerate, "
                "not a muzzle" % (muzzle_tag, vlen(delta)))
            rec["barrelForward"] = value(None, UNKNOWN, "no usable muzzle offset")
        else:
            rec["muzzleOrigin"] = value(
                [round(v, 4) for v in local_org], STRONG,
                "%s resolved through the idle pose, in %s space"
                % (muzzle_tag, frame_name),
                {"tag": muzzle_tag, "distanceFromFrameIn": round(vlen(local_org), 4)})

            # Three independent estimates of the bore direction.
            #   A  the bore line: weapon frame origin -> muzzle tag
            #   B  the muzzle tag's OWN forward axis (+x, Quake convention)
            #   C  the longest side of the weapon body's own bind-pose AABB,
            #      sign-resolved by A
            dir_pos = CA.vec_norm(basis_to_local(w_bas, CA.vec_sub(m_org, w_org)))
            dir_rot = CA.vec_norm(basis_to_local(
                w_bas, CA.vec_norm((m_bas[0], m_bas[1], m_bas[2]))))
            ests = {"fromPosition": dir_pos, "fromRotation": dir_rot}
            body_bas = basis(p0, mesh.name) if mesh is not None else None
            if body_bas is not None:
                ax, ln, axi = aabb_long_axis(mesh)
                # bone-local -> model space -> weapon frame
                ax = CA.vec_norm(basis_to_local(w_bas, CA.mat_apply(body_bas, ax)))
                if CA.vec_dot(ax, dir_pos) < 0.0:
                    ax = (-ax[0], -ax[1], -ax[2])
                ests["fromBodyAabb"] = ax
                rec["bodyLengthIn"] = round(ln, 3)
                rec["bodyLongAxis"] = axi

            pairs = sorted(ests)
            agrees = {}
            for i in range(len(pairs)):
                for j in range(i + 1, len(pairs)):
                    agrees["%s~%s" % (pairs[i], pairs[j])] = round(
                        CA.angle_between_deg(ests[pairs[i]], ests[pairs[j]]), 3)
            n_agree = sum(1 for v in agrees.values() if v <= MUZZLE_AGREE_DEG)
            extra = {"estimates": dict((k, [round(x, 6) for x in v])
                                       for k, v in ests.items()),
                     "agreementDeg": agrees}
            if n_agree >= 2:
                conf, ev = PROVEN, ("%d of %d independent estimates agree within "
                                    "%.0f deg" % (n_agree, len(agrees), MUZZLE_AGREE_DEG))
            elif n_agree == 1:
                conf, ev = STRONG, "two estimates agree; a third dissents"
            else:
                conf, ev = WEAK, "no two estimates agree within %.0f deg" % MUZZLE_AGREE_DEG
            rec["barrelForward"] = value([round(v, 6) for v in dir_pos], conf,
                                         "%s, via %s" % (ev, muzzle_tag), extra)
    else:
        rec["muzzleOrigin"] = value(None, UNKNOWN, "no muzzle-like tag on the gun rig")
        rec["barrelForward"] = value(None, UNKNOWN, "no muzzle-like tag on the gun rig")

    # -------------------------------------------- Phase 3: rear (dominant) grip
    # CoD viewmodels are authored right-handed: the trigger hand is bip01 r hand.
    def measure(bone_name):
        """(mean over idle, idle spread, worst excursion during firing)."""
        il = offsets(bone_name, idle_poses)
        if not il:
            return None, None, None
        mean = tuple(sum(l[i] for l in il) / len(il) for i in range(3))
        spread = max(vlen(CA.vec_sub(l, mean)) for l in il)
        fl = offsets(bone_name, fire_poses)
        exc = max([vlen(CA.vec_sub(l, mean)) for l in fl], default=0.0)
        return mean, spread, exc

    mean, spread, exc = measure("bip01 r hand")
    if mean is not None:
        conf = STRONG if exc <= GRIP_STABLE_IN else WEAK
        rec["rearGripOffset"] = value(
            [round(v, 4) for v in mean], conf,
            "bip01 r hand relative to %s across %d idle frames (spread %.3f in); "
            "worst excursion over %d firing frames %.3f in"
            % (frame_name, len(idle_poses), spread, len(fire_poses), exc),
            {"idleSpreadIn": round(spread, 4),
             "fireExcursionIn": round(exc, 4),
             "distanceIn": round(vlen(mean), 4)})
        rb = basis(p0, "bip01 r hand")
        if rb is not None:
            rel = CA.mat_mul(rb, mat_transpose(w_bas))
            q = mat_to_quat(rel)
            rec["rearGripRotation"] = value(
                [round(v, 6) for v in q], conf,
                "bip01 r hand basis relative to tag_weapon basis in the idle pose",
                {"format": "quaternion xyzw, hand-from-weapon"})

    # ---------------------------------------------- Phase 4: support hand
    two_handed = str(fields.get("twoHanded", "")) == "1"
    if barrelless:
        why = ("vrClass %s: the off hand is not placed on a barrel, so an "
               "along-axis/radius decomposition has no meaning" % vrclass)
        rec["supportAlongAxis"] = value(None, NOT_APPLICABLE, why)
        rec["supportRadius"] = value(None, NOT_APPLICABLE, why)
        s_mean, _s_spread, _s_exc = measure("bip01 l hand")
        if s_mean is not None:
            rec["supportOffset"] = value(
                [round(v, 4) for v in s_mean], WEAK,
                "bip01 l hand in the idle pose; recorded for reference only, %s" % why)
    else:
        s_mean, s_spread, s_exc = measure("bip01 l hand")
        if s_mean is not None and rec["barrelForward"]["value"] is not None:
            axis = tuple(rec["barrelForward"]["value"])
            along = CA.vec_dot(s_mean, axis)
            radial = CA.vec_sub(s_mean,
                                (axis[0] * along, axis[1] * along, axis[2] * along))
            stable = s_exc <= SUPPORT_STABLE_IN
            conf = (STRONG if stable else WEAK) if two_handed else WEAK
            note = ("bip01 l hand relative to %s across %d idle frames "
                    "(spread %.3f in); worst excursion over %d firing frames "
                    "%.3f in; reload, melee, rechamber, last-shot and "
                    "raise/drop animations excluded by construction"
                    % (frame_name, len(idle_poses), s_spread,
                       len(fire_poses), s_exc))
            if not two_handed:
                note += ("; the weapon file does not mark this weapon twoHanded, "
                         "so the off hand is presentation, not a grip")
            rec["supportOffset"] = value([round(v, 4) for v in s_mean], conf, note,
                                         {"idleSpreadIn": round(s_spread, 4),
                                          "fireExcursionIn": round(s_exc, 4)})
            rec["supportAlongAxis"] = value(round(along, 4), conf,
                                            note + "; projected on the barrel axis")
            rec["supportRadius"] = value(
                round(vlen(radial), 4), conf,
                note + "; perpendicular distance from the bore line",
                {"radialVector": [round(v, 4) for v in radial]})

    # ------------------------------------------------- Phase 5: sight line
    # How far the hands actually travel in the excluded animations. This is
    # the evidence that the exclusion is load-bearing rather than decorative:
    # if a reload moved the support hand by a tenth of an inch, filtering it
    # out would be theatre.
    trans_names = []
    worst_support = 0.0
    worst_grip = 0.0
    base_s = rec["supportOffset"]["value"]
    base_g = rec["rearGripOffset"]["value"]
    for fld in TRANSIENT_ANIM_FIELDS:
        nm = fields.get(fld, "")
        if not nm or nm in trans_names or nm in rec.get("steadyAnims", []):
            continue
        a = assets.anim(nm)
        if a is None or not drives_this_weapon(a):
            continue
        trans_names.append(nm)
        for f in steady_frames(a, cap=10):
            tp = pose_model_space(bones, [(a, f)])
            o = pos(tp, frame_name)
            bb = basis(tp, frame_name)
            if o is None:
                continue
            if base_s is not None:
                s = pos(tp, "bip01 l hand")
                if s is not None:
                    d = vlen(CA.vec_sub(basis_to_local(bb, CA.vec_sub(s, o)),
                                        tuple(base_s)))
                    worst_support = max(worst_support, d)
            if base_g is not None:
                g = pos(tp, "bip01 r hand")
                if g is not None:
                    d = vlen(CA.vec_sub(basis_to_local(bb, CA.vec_sub(g, o)),
                                        tuple(base_g)))
                    worst_grip = max(worst_grip, d)
    if trans_names:
        rec["excludedAnims"] = trans_names
        rec["excludedSupportExcursionIn"] = round(worst_support, 3)
        rec["excludedGripExcursionIn"] = round(worst_grip, 3)

    # A rig that ships a named sight bone answers the sight question outright.
    # "rear"/"front" alone are not sight words: they matched `mosin_boltfront`,
    # which is part of the bolt.
    sight_bones = [b.name for b in gun_parts.bones
                   if any(k in b.name.lower()
                          for k in ("sight", "site", "scope", "aperture"))]
    rec["sightBones"] = sight_bones
    if sight_bones and pos(p0, sight_bones[0]) is not None:
        s_org = pos(p0, sight_bones[0])
        off = basis_to_local(w_bas, CA.vec_sub(s_org, w_org))
        # A sight bone sitting exactly on the frame origin is an undriven bind
        # bone inheriting its parent, not a measurement (m1carbine's `m1site`).
        if vlen(off) > 0.05:
            rec["namedSightBone"] = {"bone": sight_bones[0],
                                     "offsetInFrame": [round(v, 4) for v in off]}
        else:
            rec["namedSightBone"] = {"bone": sight_bones[0], "offsetInFrame": None,
                                     "note": "bone is undriven and coincides with "
                                             "the weapon frame origin"}

    ads = assets.anim(fields.get("adsUpAnim", ""))
    if ads is not None and ads.num_frames > 0:
        last = ads.num_frames - 1
        ads_pose = pose_model_space(bones, [(idle, 0), (ads, last)])
        a_w_org = pos(ads_pose, frame_name)
        a_w_bas = basis(ads_pose, frame_name)
        if a_w_org is not None:
            # The eye sits at the viewmodel origin looking down +x (Quake).
            eye = (0.0, 0.0, 0.0)
            view_fwd = (1.0, 0.0, 0.0)
            sight_off = basis_to_local(a_w_bas, CA.vec_sub(eye, a_w_org))
            sight_fwd = CA.vec_norm(basis_to_local(a_w_bas, view_fwd))
            rec["sightOffset"] = value(
                [round(v, 4) for v in sight_off], STRONG,
                "eye position relative to %s with %s applied at its last "
                "frame, in %s space" % (frame_name, ads.name, frame_name),
                {"distanceIn": round(vlen(sight_off), 4)})
            ev = "view forward in %s space with %s applied" % (frame_name, ads.name)
            extra = {}
            if rec["barrelForward"]["value"] is not None:
                dev = CA.angle_between_deg(sight_fwd, tuple(rec["barrelForward"]["value"]))
                extra["barrelDeviationDeg"] = round(dev, 4)
                ev += "; deviates from the derived barrel axis by %.3f deg" % dev
                rec["physicalSightLine"] = PROVEN if dev <= MUZZLE_AGREE_DEG else STRONG
            else:
                rec["physicalSightLine"] = STRONG
            rec["sightForward"] = value([round(v, 6) for v in sight_fwd],
                                        STRONG, ev, extra)
            hip_pose = pose_model_space(bones, [(idle, 0), (ads, 0)])
            h_org = pos(hip_pose, frame_name)
            if h_org is not None:
                rec["adsTravelIn"] = round(vlen(CA.vec_sub(a_w_org, h_org)), 4)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--main", default=CA.GAME_MAIN)
    args = ap.parse_args()

    fs = CA.Pk3Fs(args.main)
    assets = Assets(fs)
    weapons = EW.load_sp_weapons(args.main)

    out = []
    for name, (_pak, fields) in sorted(weapons.items()):
        vrclass = EW.vr_class(name, fields)
        if isinstance(vrclass, (tuple, list)):
            vrclass = vrclass[0]
        out.append(extract_weapon(name, fields, assets, vrclass))
    fs.close()

    counts = {}
    for k in ("muzzleOrigin", "barrelForward", "rearGripOffset", "rearGripRotation",
              "supportAlongAxis", "supportRadius", "sightOffset", "sightForward"):
        c = {}
        for r in out:
            c[r[k]["confidence"]] = c.get(r[k]["confidence"], 0) + 1
        counts[k] = c

    doc = {"weapons": out, "confidenceCounts": counts,
           "note": "Asset-derived priors. Not hardware-verified."}
    if args.json:
        with open(args.json, "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
        print("wrote", args.json)
    if args.md:
        write_md(args.md, out, counts)
        print("wrote", args.md)

    print()
    for k, c in counts.items():
        print("%-18s %s" % (k, ", ".join("%s=%d" % kv for kv in sorted(c.items()))))
    return 0


def write_md(path, out, counts):
    L = []
    L.append("# Weapon geometry inventory\n")
    L.append("Derived from the shipped CoD1 pk3s by `tools/extract_geometry.py`. "
             "Every number here is an **asset-derived prior**, not a hardware "
             "measurement: it says where the game's own artists put the hands, "
             "the muzzle and the sight line relative to each weapon, which is a "
             "far better starting point than AUTO but is still subject to the "
             "headset check.\n")
    L.append("Units are inches (CoD units; 1 unit = 1 inch). Vectors are in "
             "**tag_weapon space** unless stated: +x forward along the weapon, "
             "+y left, +z up, matching the engine's Quake axes.\n")

    L.append("\n## Confidence roll-up\n")
    L.append("| field | PROVEN | STRONG | WEAK | UNKNOWN |")
    L.append("|---|---|---|---|---|")
    for k, c in counts.items():
        L.append("| %s | %d | %d | %d | %d |"
                 % (k, c.get(PROVEN, 0), c.get(STRONG, 0), c.get(WEAK, 0),
                    c.get(UNKNOWN, 0)))

    L.append("\n## Per-weapon semantic tags\n")
    L.append("| weapon | gun parts | tags found on the gun rig |")
    L.append("|---|---|---|")
    for r in out:
        L.append("| %s | %s | %s |" % (r["weapon"], r.get("gunParts") or "-",
                                       ", ".join(r.get("gunTagNames") or []) or "-"))

    L.append("\n## Muzzle and barrel axis\n")
    L.append("| weapon | frame bone | muzzle tag | muzzle origin (in) | dist | barrel forward | worst pair-disagreement deg | conf |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in out:
        mo, bf = r["muzzleOrigin"], r["barrelForward"]
        L.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["weapon"], r.get("weaponFrameBone") or "-", mo.get("tag") or "-",
            fmt(mo["value"]), fmt1(mo.get("distanceFromFrameIn")),
            fmt(bf["value"]), worst_agree(bf), bf["confidence"]))

    L.append("\n## Grips\n")
    L.append("| weapon | rear grip offset (in) | wander | conf | support offset (in) | along | radius | conf |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in out:
        rg, so = r["rearGripOffset"], r["supportOffset"]
        L.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["weapon"], fmt(rg["value"]), fmt1(rg.get("wanderIn")), rg["confidence"],
            fmt(so["value"]), fmt1(r["supportAlongAxis"]["value"]),
            fmt1(r["supportRadius"]["value"]), so["confidence"]))

    L.append("\n## Sight line\n")
    L.append("| weapon | sight offset (eye rel. weapon, in) | sight forward | barrel deviation deg | PHYSICAL_SIGHT_LINE |")
    L.append("|---|---|---|---|---|")
    for r in out:
        so, sf = r["sightOffset"], r["sightForward"]
        L.append("| %s | %s | %s | %s | %s |" % (
            r["weapon"], fmt(so["value"]), fmt(sf["value"]),
            fmt1(sf.get("barrelDeviationDeg")), r["physicalSightLine"]))

    L.append("\n## Evidence\n")
    for r in out:
        L.append("\n### %s  (%s)\n" % (r["weapon"], r["vrClass"]))
        L.append("- status: `%s`; gun parts `%s`; hand parts `%s`"
                 % (r["status"], r.get("gunParts"), r.get("handParts")))
        for k in ("muzzleOrigin", "barrelForward", "rearGripOffset",
                  "rearGripRotation", "supportAlongAxis", "supportRadius",
                  "sightOffset", "sightForward"):
            v = r[k]
            L.append("- **%s** = `%s`  [%s] -- %s"
                     % (k, fmt(v["value"]), v["confidence"], v["evidence"]))
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def fmt(v):
    if v is None:
        return "AUTO"
    if isinstance(v, (int, float)):
        return "%.3f" % v
    return "(" + ", ".join("%.3f" % x for x in v) + ")"


def fmt1(v):
    return "-" if v is None else ("%.3f" % v)


def worst_agree(field):
    a = field.get("agreementDeg")
    if not isinstance(a, dict) or not a:
        return "-"
    k = max(a, key=lambda x: a[x])
    return "%.2f (%s)" % (a[k], k.replace("from", ""))


if __name__ == "__main__":
    sys.exit(main())
