#!/usr/bin/env python3
"""Render the sweep's A/B evidence as images a human can check at a glance.

Per probe stage:
  <stage>_overlay.png   probe frame, signal mask tinted, blob boxes drawn
  <stage>_triptych.png  baseline / probe / baseline, cropped to the action

Plus one null_overlay.png per baseline triple, so the detector's own
false-positive footprint is visible next to the real ones at the same scale.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent))
import shotlib as S  # noqa: E402
import score_shots as SC  # noqa: E402

TINT = (255, 40, 40)
CROP = (110, 620, 1440, 1440)  # the band the viewmodel actually occupies


def tint(rgb, mask, colour=TINT, alpha=0.55):
    out = rgb.astype(np.float32).copy()
    c = np.array(colour, np.float32)
    out[mask] = (1 - alpha) * out[mask] + alpha * c
    return out.astype(np.uint8)


def draw_boxes(img, comps, colour=(255, 230, 0)):
    im = Image.fromarray(img)
    d = ImageDraw.Draw(im)
    for area, sl, _ in comps[:10]:
        ys, xs = sl
        d.rectangle([xs.start, ys.start, xs.stop, ys.stop], outline=colour, width=3)
        d.text((xs.start + 4, max(0, ys.start - 14)), str(area), fill=colour)
    return np.asarray(im)


def main(root, outdir):
    root, outdir = Path(root), Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    rgb = {s: S.load_tga(S.stage_paths(root, s)[0]) for s in S.ORDER}
    L = {s: SC.luma(v) for s, v in rgb.items()}
    x0, y0, x1, y1 = CROP

    for p in S.PROBES:
        a, b = S.prev_base(p), S.next_base(p)
        m = SC.clean(SC.signal_mask(L[p], L[a], L[b]))
        comps = SC.components(m)
        ov = draw_boxes(tint(rgb[p], m), comps)
        Image.fromarray(ov).save(outdir / f"{p}_overlay.png")
        strip = np.concatenate(
            [rgb[a][y0:y1, x0:x1], rgb[p][y0:y1, x0:x1], rgb[b][y0:y1, x0:x1]], axis=1
        )
        Image.fromarray(strip).save(outdir / f"{p}_triptych.png")
        print(f"{p}: mask {int(m.sum())} px, {len(comps)} blobs")

    for i in range(1, len(S.BASES) - 1):
        a, p, b = S.BASES[i - 1], S.BASES[i], S.BASES[i + 1]
        m = SC.clean(SC.signal_mask(L[p], L[a], L[b]))
        comps = SC.components(m)
        Image.fromarray(draw_boxes(tint(rgb[p], m), comps)).save(
            outdir / f"null_{p}_overlay.png"
        )
        print(f"null {p}: mask {int(m.sum())} px, {len(comps)} blobs")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
