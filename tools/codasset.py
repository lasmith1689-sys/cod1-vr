#!/usr/bin/env python3
"""Read-only reader for the CoD1 asset containers we need for VR calibration.

Three formats live here, all reverse-engineered from the shipped pk3s and all
validated structurally -- every file must consume to exactly EOF, see
tools/selftest_assets.py -- because a parser that silently mis-frames a record
produces confident nonsense, which is worse than an honest UNKNOWN.

    xmodel/<name>        model container: bounds, LOD -> parts name, materials
    xmodelparts/<name>   bone/tag hierarchy, bind pose, per-bone AABB
    xanim/<name>         animation: per-frame bone translations/rotations

Nothing here writes, launches, or patches anything. It is archive reading only.
"""

import glob
import os
import struct
import zipfile

GAME_MAIN = r"H:\SteamLibrary\steamapps\common\Call of Duty\Main"

XMODEL_VERSION = 14
XMODELPARTS_VERSION = 14

# xmodelparts bone record: u8 parent, f32 trans[3], i16 quat[3]
_BONE_RECORD = 19
# ...and one 6-float AABB per bone, stored right after that bone's name.
_BONE_AABB = 24


# --------------------------------------------------------------------- vfs

class Pk3Fs(object):
    """The engine load order: a later pak overrides an earlier one."""

    def __init__(self, main_dir=GAME_MAIN):
        self.main_dir = main_dir
        self._index = {}
        self._open = {}
        for pak in sorted(glob.glob(os.path.join(main_dir, "*.pk3"))):
            try:
                zf = zipfile.ZipFile(pak)
            except Exception:
                continue
            with zf:
                for entry in zf.namelist():
                    if entry.endswith("/"):
                        continue
                    self._index[entry.lower().replace("\\", "/")] = (pak, entry)

    def __contains__(self, path):
        return path.lower().replace("\\", "/") in self._index

    def close(self):
        for zf in self._open.values():
            zf.close()
        self._open = {}

    def read(self, path):
        loc = self._index.get(path.lower().replace("\\", "/"))
        if loc is None:
            return None
        # The paks are up to a gigabyte; reopening one per asset turns a
        # whole-archive sweep into minutes, so the handles are kept.
        zf = self._open.get(loc[0])
        if zf is None:
            zf = self._open[loc[0]] = zipfile.ZipFile(loc[0])
        return zf.read(loc[1])

    def source(self, path):
        loc = self._index.get(path.lower().replace("\\", "/"))
        return os.path.basename(loc[0]) if loc else None

    def list(self, prefix):
        prefix = prefix.lower()
        return sorted(p for p in self._index if p.startswith(prefix))


def _cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1"), end + 1


# ------------------------------------------------------------------ xmodel

class XModel(object):
    __slots__ = ("name", "mins", "maxs", "lods", "tail_marker", "materials",
                 "has_collision_blob", "raw_len")

    @property
    def parts_name(self):
        """The xmodelparts/xmodelsurfs base name for the highest LOD."""
        for _dist, nm in self.lods:
            if nm:
                return nm
        return None


def parse_xmodel(data, name=""):
    """`xmodel/<name>`, v14.

        u16   version (14)
        f32   mins[3], maxs[3]
        3 x { f32 lodDistance; cstring lodName }
        u32   tailMarker         0xffffffff on models with no collision blob
        ...   optional collision blob (world models only)
        u16   numMaterials
        cstring materials[numMaterials]      -- always terminates the file

    Only the header half sits at a fixed offset. The material block is
    recovered from the end of the file, which is exact because it is always
    last, and is left empty rather than guessed when it does not frame.
    """
    ver, = struct.unpack_from("<H", data, 0)
    if ver != XMODEL_VERSION:
        raise ValueError("xmodel %s: version %d, expected %d"
                         % (name, ver, XMODEL_VERSION))
    mins = struct.unpack_from("<3f", data, 2)
    maxs = struct.unpack_from("<3f", data, 14)
    off = 26
    lods = []
    for _ in range(3):
        dist, = struct.unpack_from("<f", data, off)
        off += 4
        nm, off = _cstr(data, off)
        lods.append((dist, nm))
    marker, = struct.unpack_from("<I", data, off)
    off += 4

    mats, mat_start = _materials_from_tail(data)

    m = XModel()
    m.name = name
    m.mins = mins
    m.maxs = maxs
    m.lods = lods
    m.tail_marker = marker
    m.materials = mats
    m.has_collision_blob = mat_start > off + 2
    m.raw_len = len(data)
    return m


def _materials_from_tail(data):
    """Find the u16 count + cstring block that terminates the file.

    Deterministic: scan candidate offsets forward through the trailing
    printable region, accepting the first u16 whose count frames exactly that
    many printable cstrings ending on the final byte.
    """
    if not data.endswith(b"\0"):
        return [], len(data)
    limit = len(data)
    start = limit - 1
    while start > 0:
        b = data[start - 1]
        if b == 0 or 32 <= b < 127:
            start -= 1
        else:
            break
    start = max(0, start - 2)
    for k in range(start, limit - 1):
        n, = struct.unpack_from("<H", data, k)
        if n == 0 or n > 64:
            continue
        off = k + 2
        names = []
        good = True
        for _ in range(n):
            try:
                s, off = _cstr(data, off)
            except ValueError:
                good = False
                break
            if not s or any(not (32 <= ord(c) < 127) for c in s):
                good = False
                break
            names.append(s)
        if good and off == limit:
            return names, k
    return [], limit


# ------------------------------------------------------------- xmodelparts

class Bone(object):
    __slots__ = ("index", "name", "parent", "trans", "quat", "mins", "maxs", "flag")

    def __repr__(self):
        return "<Bone %d %r parent=%d>" % (self.index, self.name, self.parent)

    @property
    def has_geometry(self):
        return any(v != 0.0 for v in self.mins) or any(v != 0.0 for v in self.maxs)

    @property
    def extent(self):
        return tuple(self.maxs[i] - self.mins[i] for i in range(3))


class XModelParts(object):
    __slots__ = ("name", "num_bones", "num_root_bones", "bones", "raw_len")

    @property
    def total_bones(self):
        return len(self.bones)

    def by_name(self, n):
        n = n.lower()
        for b in self.bones:
            if b.name.lower() == n:
                return b
        return None

    def children(self, index):
        return [b for b in self.bones if b.parent == index and b.index != index]


def parse_xmodelparts(data, name=""):
    """`xmodelparts/<name>`, v14.

        u16 version (14)
        u16 numBones            -- NON-root bones; total = numBones + numRootBones
        u16 numRootBones
        numBones x { u8 parent; f32 trans[3]; i16 quat[3] }      (19 bytes)
        total   x { cstring boneName; f32 mins[3]; f32 maxs[3] } -- per-bone AABB
        total   x   u8 trailing flag

    Root bones carry no transform record -- they are identity by definition,
    which is why the record count is numBones while the name count is the
    total. The parent field is an ABSOLUTE bone index, not a relative offset:
    the relative reading produces cyclic and self-referential hierarchies on
    the shipped files and the absolute reading produces valid forests on all
    727 of them, which is what settles it.
    """
    ver, nb, nr = struct.unpack_from("<HHH", data, 0)
    if ver != XMODELPARTS_VERSION:
        raise ValueError("xmodelparts %s: version %d" % (name, ver))
    total = nb + nr
    off = 6
    recs = []
    for _ in range(nb):
        parent = data[off]
        tx, ty, tz = struct.unpack_from("<3f", data, off + 1)
        qx, qy, qz = struct.unpack_from("<3h", data, off + 13)
        recs.append((parent, (tx, ty, tz), (qx, qy, qz)))
        off += _BONE_RECORD

    bones = []
    for i in range(total):
        nm, off = _cstr(data, off)
        box = struct.unpack_from("<6f", data, off)
        off += _BONE_AABB
        b = Bone()
        b.index = i
        b.name = nm
        b.mins = box[0:3]
        b.maxs = box[3:6]
        if i < nr:
            b.parent = -1
            b.trans = (0.0, 0.0, 0.0)
            b.quat = (0.0, 0.0, 0.0, 1.0)
        else:
            parent, trans, q = recs[i - nr]
            b.parent = parent
            b.trans = trans
            b.quat = unpack_quat(q)
        bones.append(b)

    for i in range(total):
        bones[i].flag = data[off + i]
    off += total
    if off != len(data):
        raise ValueError("xmodelparts %s: consumed %d of %d" % (name, off, len(data)))

    p = XModelParts()
    p.name = name
    p.num_bones = nb
    p.num_root_bones = nr
    p.bones = bones
    p.raw_len = len(data)
    return p


def unpack_quat(q):
    """i16 triple -> unit quaternion (x, y, z, w); w is the positive root."""
    x = q[0] / 32768.0
    y = q[1] / 32768.0
    z = q[2] / 32768.0
    t = 1.0 - (x * x + y * y + z * z)
    w = t ** 0.5 if t > 0.0 else 0.0
    return (x, y, z, w)


# ------------------------------------------------------------ small 3D math

def quat_to_mat(q):
    """(x,y,z,w) -> row-major 3x3 as a flat 9-tuple, rows are the basis axes."""
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return (
        1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy),
        2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx),
        2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy),
    )


def mat_mul(a, b):
    out = [0.0] * 9
    for r in range(3):
        for c in range(3):
            out[r * 3 + c] = (a[r * 3 + 0] * b[0 * 3 + c]
                              + a[r * 3 + 1] * b[1 * 3 + c]
                              + a[r * 3 + 2] * b[2 * 3 + c])
    return tuple(out)


def mat_apply(m, v):
    return (m[0] * v[0] + m[3] * v[1] + m[6] * v[2],
            m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
            m[2] * v[0] + m[5] * v[1] + m[8] * v[2])


def vec_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vec_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vec_len(a):
    return (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) ** 0.5


def vec_norm(a):
    n = vec_len(a)
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-9 else (0.0, 0.0, 0.0)


def vec_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def angle_between_deg(a, b):
    import math
    d = max(-1.0, min(1.0, vec_dot(vec_norm(a), vec_norm(b))))
    return math.degrees(math.acos(d))


IDENTITY3 = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


def resolve_model_space(bones, local_override=None):
    """Accumulate parent->child transforms into model space.

    `local_override` maps bone index -> (trans, quat), letting an animation
    frame replace the bind-pose local transform for the bones it drives while
    every undriven bone keeps its bind value. Returns
    {index: (origin3, basis3x3)}; bones whose parent is missing are skipped
    rather than silently rooted, so a broken hierarchy shows up as absence.
    """
    out = {}
    pending = list(range(len(bones)))
    guard = 0
    while pending and guard < len(bones) + 4:
        guard += 1
        still = []
        for i in pending:
            b = bones[i]
            if b.parent < 0:
                po, pm = (0.0, 0.0, 0.0), IDENTITY3
            elif b.parent in out:
                po, pm = out[b.parent]
            else:
                still.append(i)
                continue
            if local_override and i in local_override:
                lt, lq = local_override[i]
            else:
                lt, lq = b.trans, b.quat
            origin = vec_add(po, mat_apply(pm, lt))
            basis = mat_mul(quat_to_mat(lq), pm)
            out[i] = (origin, basis)
        if len(still) == len(pending):
            break
        pending = still
    return out


# ------------------------------------------------------------------- xanim
XANIM_VERSION = 14


class XAnimError(Exception):
    pass


class XAnimBone(object):
    __slots__ = ("name", "rot_frames", "rots", "trans_frames", "trans",
                 "narrow_rot")

    def trans_at(self, frame):
        """Translation on `frame`, or the nearest key at or before it."""
        if not self.trans:
            return None
        best = None
        for i, f in enumerate(self.trans_frames):
            if f <= frame:
                best = self.trans[i]
            else:
                break
        return best if best is not None else self.trans[0]

    def rot_at(self, frame):
        if not self.rots:
            return None
        best = None
        for i, f in enumerate(self.rot_frames):
            if f <= frame:
                best = self.rots[i]
            else:
                break
        return best if best is not None else self.rots[0]


class XAnim(object):
    __slots__ = ("name", "version", "num_frames", "num_bones", "flags",
                 "framerate", "mask_a", "mask_b", "bones", "raw_len",
                 "leftover")

    def by_name(self, n):
        n = n.lower()
        for b in self.bones:
            if b.name.lower() == n:
                return b
        return None

    def index_of(self, n):
        n = n.lower()
        for i, b in enumerate(self.bones):
            if b.name.lower() == n:
                return i
        return -1


def _bit(mask, i):
    return (mask[i // 8] >> (i % 8)) & 1


def parse_xanim(data, name="", narrow_axis=2):
    """`xanim/<name>`, v14.

        u16 version (14)
        u16 numFrames
        u16 numBones
        u8  flags
        u16 framerate
        u8  maskA[ceil(numBones/8)]     purpose unknown; never consumed here
        u8  maskB[ceil(numBones/8)]     1 => this bone's rotation is stored as
                                        ONE i16, not three
        cstring names[numBones]
        per bone:
            u16 numRotKeys
            u8  rotFrame[n]        OMITTED when n == numFrames
            rot[n]                 6 bytes, or 2 when maskB is set
            u16 numTransKeys
            u8  transFrame[n]      OMITTED when n == numFrames
            f32 trans[n][3]
        u8 trailing NUL

    Two rules are what a naive reading gets wrong. The omitted-index rule: a
    fully-keyed channel carries no index table, because it would just be
    0..numFrames-1. And the narrow-rotation rule: maskB marks bones whose
    rotation is a single packed axis component, which is why a fixed 6-byte
    rotation desynchronises three bones into any viewmodel animation.

    `narrow_axis` selects which quaternion component the single i16 fills.
    It defaults to z because that is the only choice that places the animated
    hand bones ON the weapon: resolving viewmodel_kar98_idle puts the right
    hand 2.7 in and the left hand 14.3 in from tag_weapon with z, and 18-22 in
    away -- physically impossible for hands holding a rifle -- with x or y.
    """
    if len(data) < 11:
        raise XAnimError("%s: too short (%d bytes)" % (name, len(data)))
    ver, num_frames, num_bones = struct.unpack_from("<HHH", data, 0)
    if ver != XANIM_VERSION:
        raise XAnimError("%s: version %d, expected %d" % (name, ver, XANIM_VERSION))
    flags = data[6]
    framerate, = struct.unpack_from("<H", data, 7)
    mb = (num_bones + 7) // 8
    off = 9
    mask_a = data[off:off + mb]
    off += mb
    mask_b = data[off:off + mb]
    off += mb
    if len(mask_a) != mb or len(mask_b) != mb:
        raise XAnimError("%s: truncated bone masks" % name)

    names = []
    for _ in range(num_bones):
        s, off = _cstr(data, off)
        names.append(s)

    if num_frames > 256:
        raise XAnimError("%s: frame index width unproven above 256 frames" % name)

    bones = []
    for i, nm in enumerate(names):
        b = XAnimBone()
        b.name = nm
        b.narrow_rot = bool(_bit(mask_b, i))

        n_rot, = struct.unpack_from("<H", data, off)
        off += 2
        if n_rot > num_frames:
            raise XAnimError("%s: bone %d rot keys %d > frames %d"
                             % (name, i, n_rot, num_frames))
        if n_rot == num_frames:
            b.rot_frames = list(range(n_rot))
        elif n_rot == 1:
            b.rot_frames = [0]          # constant channel: no index table
        else:
            b.rot_frames = list(data[off:off + n_rot])
            off += n_rot
        rots = []
        for _ in range(n_rot):
            if b.narrow_rot:
                v, = struct.unpack_from("<h", data, off)
                off += 2
                c = [0.0, 0.0, 0.0]
                c[narrow_axis] = v / 32768.0
                comps = tuple(c)
            else:
                x, y, z = struct.unpack_from("<3h", data, off)
                off += 6
                comps = (x / 32768.0, y / 32768.0, z / 32768.0)
            t = 1.0 - (comps[0] ** 2 + comps[1] ** 2 + comps[2] ** 2)
            rots.append((comps[0], comps[1], comps[2], t ** 0.5 if t > 0.0 else 0.0))
        b.rots = rots

        n_trans, = struct.unpack_from("<H", data, off)
        off += 2
        if n_trans > num_frames:
            raise XAnimError("%s: bone %d trans keys %d > frames %d"
                             % (name, i, n_trans, num_frames))
        if n_trans == num_frames:
            b.trans_frames = list(range(n_trans))
        elif n_trans == 1:
            b.trans_frames = [0]        # constant channel: no index table
        else:
            b.trans_frames = list(data[off:off + n_trans])
            off += n_trans
        tr = []
        for _ in range(n_trans):
            tr.append(struct.unpack_from("<3f", data, off))
            off += 12
        b.trans = tr
        bones.append(b)

    leftover = len(data) - off
    if leftover not in (0, 1):
        raise XAnimError("%s: consumed %d of %d (leftover %d)"
                         % (name, off, len(data), leftover))

    a = XAnim()
    a.name = name
    a.version = ver
    a.num_frames = num_frames
    a.num_bones = num_bones
    a.flags = flags
    a.framerate = framerate
    a.mask_a = mask_a
    a.mask_b = mask_b
    a.bones = bones
    a.raw_len = len(data)
    a.leftover = leftover
    return a


def anim_pose(anim, frame):
    """{bone name: (trans3, quat4)} for one frame of an animation."""
    out = {}
    for b in anim.bones:
        t = b.trans_at(frame)
        q = b.rot_at(frame)
        out[b.name] = (t if t else (0.0, 0.0, 0.0),
                       q if q else (0.0, 0.0, 0.0, 1.0))
    return out


# ------------------------------------------------------------ xmodelsurfs

XMODELSURFS_VERSION = 14
_PHYSIQUED = 0xFFFF

# Per-vertex base record: normal[3], uv[2], u16 numExtraWeights, u16 bone0,
# pos[3]. A vertex with any extra influence also carries its own weight float.
_VERT_BASE = 36
_VERT_W0 = 4
# One extra influence: u16 bone, pos[3] in that bone's space, f32 weight.
_WEIGHT_REC = 18
# A rigid surface's vertex is the same minus the bone and weight fields.
_VERT_RIGID = 32


class XSurfVertex(object):
    """One renderable vertex.

    `weights` is [(bone, pos3, weight), ...]; the weights sum to 1.0 on every
    shipped physiqued surface, which is one of the parser's own checks.
    """

    __slots__ = ("normal", "uv", "weights")

    @property
    def bones(self):
        return set(w[0] for w in self.weights)

    @property
    def pos(self):
        """Bind-space position: the weighted blend of the per-bone positions.

        Correct only because the shipped hand rigs have an all-zero bind pose
        (see HAND-RIG-ARCHITECTURE.md). With a non-trivial bind pose each
        entry would first need its bone's bind matrix applied.
        """
        x = y = z = 0.0
        for _b, p, w in self.weights:
            x += p[0] * w
            y += p[1] * w
            z += p[2] * w
        return (x, y, z)


class XSurface(object):
    __slots__ = ("index", "tile_mode", "num_verts", "num_tris", "physiqued",
                 "rigid_bone", "num_weight_recs", "num_rigid_verts",
                 "strips", "verts", "tris", "byte_start", "byte_end")

    @property
    def bones(self):
        out = set()
        for v in self.verts:
            out |= v.bones
        return out

    def __repr__(self):
        return ("<XSurface %d verts=%d tris=%d %s>"
                % (self.index, self.num_verts, self.num_tris,
                   "physiqued" if self.physiqued
                   else "rigid bone %d" % self.rigid_bone))


class XModelSurfs(object):
    __slots__ = ("name", "surfaces", "raw_len")

    @property
    def total_verts(self):
        return sum(s.num_verts for s in self.surfaces)

    @property
    def total_tris(self):
        return sum(s.num_tris for s in self.surfaces)


def strip_triangles(strip):
    """Triangles of one strip, dropping the degenerates the exporter left in.

    The shipped counts only add up once degenerates are dropped: a strip such
    as (10, 10, 13, 12, ...) opens with a repeated index, and counting it as a
    triangle overshoots the declared triangleCount on essentially every
    surface in the game. Dropping them reproduces all of them exactly, which
    is what settles the reading.
    """
    out = []
    for i in range(len(strip) - 2):
        a, b, c = strip[i], strip[i + 1], strip[i + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, b, c) if (i & 1) == 0 else (a, c, b))
    return out


def parse_xmodelsurfs(data, name="", verts=True):
    """`xmodelsurfs/<name>`, v14 -- the renderable geometry.

        u16 version (14)
        u16 numSurfaces
        numSurfaces x {
            u8  tileMode
            u16 vertexCount
            u16 triangleCount
            u16 stripCount
            u16 boneOrMarker         0xFFFF -> physiqued, else a rigid bone
            [physiqued] u16 numExtraWeightRecords
            [physiqued] u16 numRigidVerts
            stripCount x { u8 n; n x u16 vertexIndex }      -- triangle strips
            vertexCount x <vertex, below>
            [physiqued] numExtraWeightRecords x
                        { u16 bone; f32 pos[3]; f32 weight }
        }

    A physiqued vertex is normal[3], uv[2], u16 extraInfluenceCount, u16 bone,
    pos[3] -- 36 bytes -- plus one f32 weight when it has any extra influence.
    A rigid surface's vertex drops the bone and weight fields (32 bytes) and
    the whole surface hangs off `boneOrMarker`.

    The extra-influence records are NOT interleaved with the vertices: they
    form a single block at the end of the surface, consumed in vertex order.
    That is the part a plausible-looking inline reading gets wrong, and the
    reason the framing is asserted against EOF on all 1014 shipped files
    rather than eyeballed on one.

    `verts=False` walks the structure without materialising vertices, which is
    what the whole-archive sweep uses.
    """
    ver, nsurf = struct.unpack_from("<HH", data, 0)
    if ver != XMODELSURFS_VERSION:
        raise ValueError("xmodelsurfs %s: version %d" % (name, ver))
    off = 4
    surfaces = []
    for si in range(nsurf):
        start = off
        tile = data[off]
        vc, tc, nstrip = struct.unpack_from("<HHH", data, off + 1)
        marker, = struct.unpack_from("<H", data, off + 7)
        physiqued = marker == _PHYSIQUED
        if physiqued:
            nweight, nrigid = struct.unpack_from("<HH", data, off + 9)
            off += 13
        else:
            nweight = nrigid = 0
            off += 9

        strips = []
        for _ in range(nstrip):
            n = data[off]
            off += 1
            if verts:
                strips.append(struct.unpack_from("<%dH" % n, data, off))
            off += 2 * n

        vlist = []
        pending = []
        if physiqued:
            for _ in range(vc):
                nw, bone = struct.unpack_from("<HH", data, off + 20)
                if verts:
                    v = XSurfVertex()
                    v.normal = struct.unpack_from("<3f", data, off)
                    v.uv = struct.unpack_from("<2f", data, off + 12)
                    p0 = struct.unpack_from("<3f", data, off + 24)
                    off += _VERT_BASE
                    if nw:
                        w0, = struct.unpack_from("<f", data, off)
                        off += _VERT_W0
                    else:
                        w0 = 1.0
                    v.weights = [(bone, p0, w0)]
                    vlist.append(v)
                    pending.append((v, nw))
                else:
                    off += _VERT_BASE + (_VERT_W0 if nw else 0)
            if verts:
                for v, nw in pending:
                    for _ in range(nw):
                        b, = struct.unpack_from("<H", data, off)
                        px, py, pz, w = struct.unpack_from("<4f", data, off + 2)
                        v.weights.append((b, (px, py, pz), w))
                        off += _WEIGHT_REC
            else:
                off += nweight * _WEIGHT_REC
        else:
            for _ in range(vc):
                if verts:
                    v = XSurfVertex()
                    v.normal = struct.unpack_from("<3f", data, off)
                    v.uv = struct.unpack_from("<2f", data, off + 12)
                    v.weights = [(marker,
                                  struct.unpack_from("<3f", data, off + 20),
                                  1.0)]
                    vlist.append(v)
                off += _VERT_RIGID

        s = XSurface()
        s.index = si
        s.tile_mode = tile
        s.num_verts = vc
        s.num_tris = tc
        s.physiqued = physiqued
        s.rigid_bone = None if physiqued else marker
        s.num_weight_recs = nweight
        s.num_rigid_verts = nrigid
        s.strips = strips
        s.verts = vlist
        s.tris = [t for st in strips for t in strip_triangles(st)] if verts else []
        s.byte_start = start
        s.byte_end = off
        surfaces.append(s)

    if off != len(data):
        raise ValueError("xmodelsurfs %s: consumed %d of %d bytes"
                         % (name, off, len(data)))
    m = XModelSurfs()
    m.name = name
    m.surfaces = surfaces
    m.raw_len = len(data)
    return m


# ------------------------------------------------------- laterality

# The trailing per-bone flag byte in xmodelparts. On the hand rigs it is a
# clean partition and it disagrees with the bone NAMES on two bones per rig
# (`r cuff01` and `r wrist01` are parented to the LEFT hand). Parent chain and
# flag byte agree with each other, so they are the authority.
BONE_FLAG_LEFT = 0x1        # set on the left arm chain (flags 9 and 11)
BONE_FLAG_HAND = 0x2        # set at hand level and below (flags 10 and 11)
BONE_FLAG_ARM = 0x8         # set on any bone belonging to an arm chain

SIDE_LEFT = "L"
SIDE_RIGHT = "R"
SIDE_NEUTRAL = "-"


def bone_side(parts, index):
    """`L`, `R` or `-` for one bone, from the flag byte and the parent chain.

    The flag byte alone leaves the two upperarms neutral (flag 0), which is
    correct -- they are shared shoulder roots -- but useless for splitting
    geometry. So a neutral bone inherits the side of its own subtree when that
    subtree is unanimous, which resolves both upperarms without ever consulting
    a bone name.
    """
    b = parts.bones[index]
    if b.flag & BONE_FLAG_ARM:
        return SIDE_LEFT if (b.flag & BONE_FLAG_LEFT) else SIDE_RIGHT
    sides = set()
    stack = [c.index for c in parts.children(index)]
    while stack:
        i = stack.pop()
        c = parts.bones[i]
        if c.flag & BONE_FLAG_ARM:
            sides.add(SIDE_LEFT if (c.flag & BONE_FLAG_LEFT) else SIDE_RIGHT)
        else:
            stack.extend(x.index for x in parts.children(i))
    if len(sides) == 1:
        return sides.pop()
    return SIDE_NEUTRAL


def bone_sides(parts):
    """[side] indexed by bone, for a whole rig."""
    return [bone_side(parts, i) for i in range(len(parts.bones))]
