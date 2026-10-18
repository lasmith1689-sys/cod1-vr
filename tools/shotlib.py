#!/usr/bin/env python3
"""Shared loaders for the GL surface sweep evidence.

The sweep emits, per stage, one uncompressed 24bpp TGA framebuffer and one CSV
draw census. Nothing here interprets: it only turns bytes into arrays.
"""

import csv
import struct
from pathlib import Path

import numpy as np

MV = [f"mv{i}" for i in range(16)]
US = [f"us{i}" for i in range(16)]
PJ = ["pj0", "pj5", "pj8", "pj9", "pj10", "pj11", "pj12", "pj13", "pj14"]

# Stage -> the class the probe moved, and the delta it asked for.
# (dx, dy, dz) in CoD units, yaw in degrees, applied about the viewmodel origin.
STAGES = {
    "base0":     (None,       (0.0, 0.0, 0.0), 0.0),
    "gun_x":     ("gun",      (3.937, 0.0, 0.0), 0.0),
    "base1":     (None,       (0.0, 0.0, 0.0), 0.0),
    "gun_yaw":   ("gun",      (0.0, 0.0, 0.0), 15.0),
    "base2":     (None,       (0.0, 0.0, 0.0), 0.0),
    "handR_x":   ("hand_R",   (3.937, 0.0, 0.0), 0.0),
    "base3":     (None,       (0.0, 0.0, 0.0), 0.0),
    "handL_x":   ("hand_L",   (-3.937, 0.0, 0.0), 0.0),
    "base4":     (None,       (0.0, 0.0, 0.0), 0.0),
    "sleeveR_x": ("sleeve_R", (3.937, 0.0, 0.0), 0.0),
    "base5":     (None,       (0.0, 0.0, 0.0), 0.0),
    "sleeveL_x": ("sleeve_L", (-3.937, 0.0, 0.0), 0.0),
    "base6":     (None,       (0.0, 0.0, 0.0), 0.0),
}

# The stage order as the sweep ran it. Every probe stage is bracketed by the
# baseline immediately before and the baseline immediately after, so the
# animation noise floor is measurable on the very same neighbourhood.
ORDER = list(STAGES)

PROBES = [s for s in ORDER if STAGES[s][0] is not None]
BASES = [s for s in ORDER if STAGES[s][0] is None]


def prev_base(stage):
    return ORDER[ORDER.index(stage) - 1]


def next_base(stage):
    return ORDER[ORDER.index(stage) + 1]


def load_tga(path):
    """Return an (H, W, 3) uint8 RGB array, top row first."""
    raw = Path(path).read_bytes()
    idlen, cmtype, imtype, _, _, _, _, _, w, h, bpp, desc = struct.unpack(
        "<BBBHHBHHHHBB", raw[:18]
    )
    if imtype != 2 or bpp != 24:
        raise ValueError(f"{path}: unsupported TGA (type {imtype}, {bpp}bpp)")
    off = 18 + idlen + cmtype * 0  # no colour map on type 2
    px = np.frombuffer(raw, dtype=np.uint8, count=w * h * 3, offset=off)
    img = px.reshape(h, w, 3)[:, :, ::-1]  # BGR -> RGB
    if not (desc & 0x20):  # origin bottom-left: flip to top-down
        img = img[::-1]
    return np.ascontiguousarray(img)


def load_census(path):
    """Return (rows, header). Rows are dicts of str; callers coerce."""
    rows = []
    with open(path, newline="") as f:
        lines = [ln for ln in f if not ln.startswith("#")]
    rdr = csv.DictReader(lines)
    for r in rdr:
        rows.append(r)
    return rows


def mat4(row, keys):
    """Column-major GL matrix -> 4x4 numpy, so that v_out = M @ v_in."""
    m = np.array([float(row[k]) for k in keys], dtype=np.float64)
    return m.reshape(4, 4).T


def proj4(row):
    """Rebuild the full 4x4 projection from the nine recorded entries."""
    p = np.zeros((4, 4), dtype=np.float64)
    p[0, 0] = float(row["pj0"])
    p[1, 1] = float(row["pj5"])
    p[0, 2] = float(row["pj8"])
    p[1, 2] = float(row["pj9"])
    p[2, 2] = float(row["pj10"])
    p[3, 2] = float(row["pj11"])
    p[2, 3] = float(row["pj14"])
    # pj12/pj13 are the (unused) 4th-column x/y; carry them anyway.
    p[0, 3] = float(row["pj12"])
    p[1, 3] = float(row["pj13"])
    return p


def stage_paths(root, stage):
    root = Path(root)
    return (
        root / f"codvr_shot_sweep_{stage}.tga",
        root / f"codvr_draws_sweep_{stage}.csv",
    )
