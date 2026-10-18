#!/usr/bin/env python3
"""Score the REAL flat A/B sweep screenshots for surface isolation.

The question this answers is not "did pixels change" -- idle animation,
wandering NPCs and a fading title card all change pixels. It is:

    did the pixels that the probe was supposed to move actually move,
    and did nothing else move with them?

Method -- three-frame differencing against a BRACKETING pair of baselines.

Every probe stage P sits between two baseline stages A and B, both captured
with the transform OFF:

        A ......... P ......... B
      base_n     probe      base_n+1

A pixel that the transform is responsible for differs from A *and* from B,
while A and B agree with each other. A pixel that mere animation moved
(a running soldier, a fading caption) differs between A and B as well. So:

    signal = |P-A| > T  AND  |P-B| > T,   T = max(k*|A-B|, T0)

is the transform's own footprint, with the animation noise floor subtracted
using frames captured on either side of it rather than an assumed model of
what "drift" looks like. T is not a guess either, and it is not one number:
it is |A-B| at each pixel, so a violently-moving soldier raises the bar only
where he actually is and the quiet region containing the viewmodel keeps its
full sensitivity.

Displacement is measured on the ACTUAL RENDERED GEOMETRY, per block, never
from a matrix origin -- see shift_field(). It is reported as a distribution
because one number cannot describe it: the transform is rigid in EYE space,
but projection turns that into a pixel displacement scaling as 1/depth, and
these surfaces span roughly 6 to 60 units of depth. The authoritative
displacement measurement belongs in the probe, where real vertices are in
hand; this module corroborates WHERE things moved, which is what the
screenshots can actually settle.
"""

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import shotlib as S  # noqa: E402

# Screen regions are reported in the captured framebuffer's own pixels.
# Everything below is derived; there are no hand-placed masks.

MIN_BLOB = 400  # px; smaller connected components are speckle, not a surface


def luma(img):
    """Perceptual-ish scalar image, float32, 0..255."""
    return (
        0.299 * img[:, :, 0] + 0.587 * img[:, :, 1] + 0.114 * img[:, :, 2]
    ).astype(np.float32)


FLOOR_MIN = 10.0  # luma steps; below this a difference is compression/dither
FLOOR_K = 1.6  # a real change must beat the local animation floor by this much
FLOOR_GROW = 3  # px; a soldier who moved also disturbs his own silhouette edge


def noise_floor(before, after):
    """PER-PIXEL animation floor, measured on the two bracketing baselines.

    A single global threshold is the wrong instrument here: the frame contains
    a handful of violently-moving regions (running soldiers, a fading title
    card) and a large quiet region that includes the whole viewmodel. Any
    global quantile is either deaf in the quiet region or blind in the loud
    one. |A-B| at each pixel is the floor for that pixel, grown slightly so a
    silhouette that shifted a few pixels does not leak past its own edge.
    """
    from scipy import ndimage

    d = np.abs(before - after)
    grown = ndimage.maximum_filter(d, size=2 * FLOOR_GROW + 1)
    return np.maximum(FLOOR_K * grown, FLOOR_MIN)


def signal_mask(probe, before, after):
    """Pixels the transform moved: differ from both baselines, which agree."""
    thr = noise_floor(before, after)
    return (np.abs(probe - before) > thr) & (np.abs(probe - after) > thr)


def components(mask, min_area=MIN_BLOB):
    """Label 8-connected components, return [(area, slice_y, slice_x, mask)]."""
    from scipy import ndimage

    lab, n = ndimage.label(mask, structure=np.ones((3, 3), dtype=bool))
    out = []
    if n == 0:
        return out
    areas = ndimage.sum(mask, lab, index=np.arange(1, n + 1))
    objs = ndimage.find_objects(lab)
    for i, (area, sl) in enumerate(zip(areas, objs), start=1):
        if area < min_area:
            continue
        out.append((int(area), sl, lab[sl] == i))
    out.sort(key=lambda t: -t[0])
    return out


def clean(mask, iters=1):
    """Drop isolated speckle without eroding a real surface footprint."""
    from scipy import ndimage

    m = ndimage.binary_opening(mask, structure=np.ones((3, 3), bool), iterations=iters)
    return ndimage.binary_closing(m, structure=np.ones((5, 5), bool))


MIN_SHIFT = 3  # px; below this the estimator cannot separate motion from noise


MIN_SUPPORT = 250   # absolute floor on arrived pixels
SUPPORT_FRAC = 0.20  # ...and at least this fraction of the surface's own mask


def best_shift(probe, base, mask, max_shift=460):
    """Displacement of the ACTUAL RENDERED GEOMETRY, from the pixels alone.

    For a candidate shift s, the pixels that can testify are the ARRIVED set

        A(s) = mask(x) AND mask(x - s)

    -- pixels that changed, and whose source position also changed. If the
    probe really moved this surface by s then the object landed on A(s) and
    came from mask(x-s), so probe(x) == base(x-s) there and the residual
    collapses. At -s, A(s) is the region the object LEFT: the object is gone
    from the probe there, and the residual stays high. So magnitude and sign
    fall out of one search, with no symmetric-autocorrelation ambiguity to
    break afterwards.

    Every term is a cross-correlation, so the whole shift plane is evaluated
    by FFT rather than by walking candidates:

        SSD(s) = SUM m(x) m(x-s) [probe(x) - base(x-s)]^2
               = (m.p^2) * m  -  2 (m.p) * (m.b)  +  m * (m.b^2)

    This reads only rendered pixels. No vertex buffer, no matrix origin, and
    nothing that can go singular when a draw's modelview origin sits at zero
    clip-w.
    """
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return None
    pad = max_shift + 8
    y0 = max(0, ys.min() - pad); y1 = min(base.shape[0], ys.max() + 1 + pad)
    x0 = max(0, xs.min() - pad); x1 = min(base.shape[1], xs.max() + 1 + pad)
    m = mask[y0:y1, x0:x1].astype(np.float64)
    p = probe[y0:y1, x0:x1].astype(np.float64)
    b = base[y0:y1, x0:x1].astype(np.float64)

    n = _corr(m, m)
    ssd = _corr(m * p * p, m) - 2.0 * _corr(m * p, m * b) + _corr(m, m * b * b)
    h, w = m.shape
    r = min(max_shift, h - 1, w - 1)
    cy, cx = h - 1, w - 1
    sl = (slice(cy - r, cy + r + 1), slice(cx - r, cx + r + 1))
    n, ssd = n[sl], ssd[sl]
    uy, ux = np.mgrid[-r : r + 1, -r : r + 1]

    # Support must scale with the surface, or the minimiser escapes to a huge
    # shift where only a sliver of pixels overlaps and a chance match wins.
    min_support = max(MIN_SUPPORT, SUPPORT_FRAC * float(mask.sum()))
    ok = (n >= min_support) & ((uy * uy + ux * ux) >= MIN_SHIFT * MIN_SHIFT)
    if not ok.any():
        return None
    rms = np.full(n.shape, np.inf)
    good = ok & (n > 0)
    rms[good] = np.sqrt(np.maximum(ssd[good], 0.0) / n[good])
    k = int(np.argmin(rms))
    sy, sx = int(uy.flat[k]), int(ux.flat[k])

    # What the same pixels looked like before the shift was undone.
    arrived = mask & _shift_mask(mask, sy, sx)
    before = float(np.abs(probe[arrived] - base[arrived]).mean())
    after = float(rms.flat[k])
    return {
        "dx": sx,
        "dy": sy,
        "magnitude_px": round(float(np.hypot(sx, sy)), 2),
        "arrived_px": int(n.flat[k]),
        "rms_after": round(after, 2),
        "mad_before": round(before, 2),
        "explained": round(1.0 - after / before, 4) if before > 1e-6 else None,
    }


BLOCK = 48
STRIDE = 32
BLOCK_COVER = 0.25  # a block must be at least this much signal to vote
DY_SEARCH = 10  # px of vertical freedom, kept only to TEST that it stays ~0


def shift_field(probe, base, mask, max_dx=460, block=BLOCK, stride=STRIDE):
    """Per-block displacement of the rendered geometry: a field, not a number.

    A single translation cannot describe any of these surfaces. The transform
    is a rigid translation in EYE space, but the projection turns that into a
    pixel displacement that scales as 1/depth, and the gun alone spans roughly
    6 to 60 units of depth. Fitting one 2-D shift to that is ill-posed, and it
    was: the optimum wandered with every change to the support rule.

    Per block the depth is nearly constant, so a local shift IS well defined.
    Each block votes, and the report is the distribution.

    The vertical freedom is deliberate. The recorded projection has pj8=pj9=0,
    so eye-space (x, y, z) -> (x+d, y, z) leaves screen y untouched at every
    depth: the displacement must be purely horizontal. dy is searched anyway,
    so that "dy came out at zero" is a measurement and not an assumption.
    """
    out = []
    H, W = mask.shape
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return out
    for by in range(max(0, ys.min() - block), min(H - block, ys.max() + 1), stride):
        for bx in range(max(0, xs.min() - block), min(W - block, xs.max() + 1), stride):
            sub = mask[by : by + block, bx : bx + block]
            cover = float(sub.mean())
            if cover < BLOCK_COVER:
                continue
            r = _block_shift(probe, base, mask, by, bx, block, max_dx)
            if r:
                out.append(r)
    return out


def _block_shift(probe, base, mask, by, bx, block, max_dx):
    """Masked SSD over dx in [-max_dx, max_dx] and dy in [-DY, DY] for one block."""
    y0 = max(0, by - DY_SEARCH); y1 = min(base.shape[0], by + block + DY_SEARCH)
    x0 = max(0, bx - max_dx); x1 = min(base.shape[1], bx + block + max_dx)
    tm = np.zeros(mask.shape, bool)
    tm[by : by + block, bx : bx + block] = mask[by : by + block, bx : bx + block]
    m = tm[y0:y1, x0:x1].astype(np.float64)
    if m.sum() < 60:
        return None
    p = probe[y0:y1, x0:x1].astype(np.float64)
    b = base[y0:y1, x0:x1].astype(np.float64)
    bm = mask[y0:y1, x0:x1].astype(np.float64)  # source must be signal too

    n = _corr(m, bm)
    ssd = _corr(m * p * p, bm) - 2.0 * _corr(m * p, bm * b) + _corr(m, bm * b * b)
    h, w = m.shape
    cy, cx = h - 1, w - 1
    ry = min(DY_SEARCH, h - 1)
    rx = min(max_dx, w - 1)
    n = n[cy - ry : cy + ry + 1, cx - rx : cx + rx + 1]
    ssd = ssd[cy - ry : cy + ry + 1, cx - rx : cx + rx + 1]
    uy, ux = np.mgrid[-ry : ry + 1, -rx : rx + 1]
    need = max(60.0, 0.35 * float(tm.sum()))
    ok = (n >= need) & ((uy * uy + ux * ux) >= MIN_SHIFT * MIN_SHIFT)
    if not ok.any():
        return None
    rms = np.full(n.shape, np.inf)
    g = ok & (n > 0)
    rms[g] = np.sqrt(np.maximum(ssd[g], 0.0) / n[g])
    k = int(np.argmin(rms))
    dy, dx = int(uy.flat[k]), int(ux.flat[k])
    # How much better than leaving it where it was?
    z = (ry, rx)
    still = np.sqrt(max(ssd[z], 0.0) / n[z]) if n[z] > 0 else np.inf
    return {
        "x": int(bx + BLOCK // 2), "y": int(by + BLOCK // 2),
        "dx": dx, "dy": dy,
        "rms": round(float(rms.flat[k]), 2),
        "rms_zero": round(float(still), 2) if np.isfinite(still) else None,
        "support": int(n.flat[k]),
    }


def summarise_field(field, min_gain=0.25):
    """Median / p95 over blocks that a shift actually explained."""
    good = [
        f for f in field
        if f["rms_zero"] and f["rms_zero"] > 1e-6
        and (1.0 - f["rms"] / f["rms_zero"]) >= min_gain
    ]
    if not good:
        return {"blocks": len(field), "voting": 0}
    dx = np.array([f["dx"] for f in good], float)
    dy = np.array([f["dy"] for f in good], float)
    mag = np.abs(dx)
    return {
        "blocks": len(field),
        "voting": len(good),
        "dx_median": float(np.median(dx)),
        "dx_p05": float(np.percentile(dx, 5)),
        "dx_p95": float(np.percentile(dx, 95)),
        "dy_median": float(np.median(dy)),
        "dy_absmax": float(np.abs(dy).max()),
        "dy_within_2px_pct": round(100.0 * float((np.abs(dy) <= 2).mean()), 1),
        "pixelShiftMedian": float(np.median(mag)),
        "pixelShiftP95": float(np.percentile(mag, 95)),
        "sign_agreement_pct": round(
            100.0 * float(max((dx > 0).mean(), (dx < 0).mean())), 1
        ),
    }


def _corr(f, g):
    """(f * g)(s) = SUM_x f(x) g(x-s), 'full' layout, zero lag at the centre."""
    from scipy.signal import fftconvolve

    return fftconvolve(f, g[::-1, ::-1], mode="full")


def _shift_mask(m, dy, dx):
    out = np.zeros_like(m)
    ys = slice(max(0, dy), m.shape[0] + min(0, dy))
    xs = slice(max(0, dx), m.shape[1] + min(0, dx))
    sy = slice(max(0, -dy), m.shape[0] + min(0, -dy))
    sx = slice(max(0, -dx), m.shape[1] + min(0, -dx))
    out[ys, xs] = m[sy, sx]
    return out


def main(root, outdir):
    root, outdir = Path(root), Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    rgb = {s: S.load_tga(S.stage_paths(root, s)[0]) for s in S.ORDER}
    L = {s: luma(v) for s, v in rgb.items()}
    H, W = L["base0"].shape

    report = {"frame": [W, H], "stages": {}, "floor": {}}

    # Raw animation drift between the baselines that bracket each probe.
    for i in range(len(S.BASES) - 1):
        a, b = S.BASES[i], S.BASES[i + 1]
        d = np.abs(L[a] - L[b])
        report["floor"][f"{a}->{b}"] = {
            "mean": round(float(d.mean()), 4),
            "p99_5": round(float(np.percentile(d, 99.5)), 2),
            "px_over_10": int((d > 10).sum()),
            "pct_over_10": round(100.0 * float((d > 10).mean()), 4),
        }

    # NULL CONTROL. The detector is handed three frames that all have the
    # transform OFF, in the same A-P-B arrangement, and must report nothing.
    # Whatever it does report here is this detector's own false-positive rate
    # on this footage -- the number every probe stage has to beat.
    report["null"] = {}
    for i in range(1, len(S.BASES) - 1):
        a, p, b = S.BASES[i - 1], S.BASES[i], S.BASES[i + 1]
        m = clean(signal_mask(L[p], L[a], L[b]))
        comps = components(m)
        report["null"][f"{a}|{p}|{b}"] = {
            "signal_px": int(m.sum()),
            "signal_pct": round(100.0 * float(m.mean()), 4),
            "blobs_over_min": len(comps),
            "largest_blob": comps[0][0] if comps else 0,
        }

    masks = {}
    for p in S.PROBES:
        a, b = S.prev_base(p), S.next_base(p)
        m = clean(signal_mask(L[p], L[a], L[b]))
        masks[p] = m
        comps = components(m)
        blobs = []
        for area, sl, sub in comps[:8]:
            ys, xs = sl
            blobs.append({
                "area": area,
                "box": [int(xs.start), int(ys.start), int(xs.stop), int(ys.stop)],
                "cx": round(float(xs.start + np.nonzero(sub)[1].mean()), 1),
                "cy": round(float(ys.start + np.nonzero(sub)[0].mean()), 1),
            })
        field = shift_field(L[p], L[a], m)
        report["stages"][p] = {
            "class": S.STAGES[p][0],
            "delta_units": S.STAGES[p][1],
            "yaw_deg": S.STAGES[p][2],
            "signal_px": int(m.sum()),
            "signal_pct": round(100.0 * float(m.mean()), 4),
            "blobs": blobs,
            "geometry": summarise_field(field),
        }
        np.save(outdir / f"field_{p}.npy",
                np.array([[f["x"], f["y"], f["dx"], f["dy"]] for f in field]))
        np.save(outdir / f"mask_{p}.npy", m)

    # Cross-stage overlap: the isolation claim in one number per pair.
    ov = {}
    ps = list(S.PROBES)
    for i in range(len(ps)):
        for j in range(i + 1, len(ps)):
            a, b = ps[i], ps[j]
            inter = int((masks[a] & masks[b]).sum())
            union = int((masks[a] | masks[b]).sum())
            ov[f"{a}|{b}"] = {
                "intersection": inter,
                "union": union,
                "iou": round(inter / union, 5) if union else 0.0,
                "frac_of_a": round(inter / max(1, int(masks[a].sum())), 5),
                "frac_of_b": round(inter / max(1, int(masks[b].sum())), 5),
            }
    report["overlap"] = ov

    (outdir / "shot_metrics.json").write_text(json.dumps(report, indent=2))
    return report, masks, rgb, L


if __name__ == "__main__":
    r, *_ = main(sys.argv[1], sys.argv[2])
    print(json.dumps(r, indent=2))
