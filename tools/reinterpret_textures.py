#!/usr/bin/env python3
"""Author a material-aware, reversible CoD1 texture replacement release.

Inventory is read-only unless --plan-out is supplied. --build is the explicit
image-processing/export step; it is never implied by inventory. No game file is
modified, no model/UV/shader is rewritten, and no generation service is called.

This deliberately reports preservation as preservation. Fonts, signs, technical
lookup textures, translucent effects, identity-sensitive faces, and skybox sets
are not falsely counted as newly interpreted artwork. Model atlases keep their
source layout and protected edges while receiving material-specific artwork.
Source-edge tests are not a substitute for an in-game/model UV inspection.
Preservation routes retain every CURRENT winning alias byte-for-byte, including
its format, alpha, mipmaps and dimensions; they never re-export stock PNGs.

Material provenance JSON: {"materials": {"brick": {"prompt": "...",
"origin": "built_in_imagegen", "sha256": "optional matching image hash"}, ...}}.
All twelve material donors must be authored and documented before --build.
Square donors are sampled with one uniform scale, crop, and wrap; they are never
resized separately along X/Y to fit a rectangular game texture.
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import csv
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import time
import zipfile
from pathlib import Path

VERSION = "codvr-material-reinterpretation-5"
PROJECT = Path(__file__).resolve().parents[1]
# An earlier HD texture pass to compare against, if one exists locally.
# Machine-specific and optional: set CODVR_PRIOR_TEXTURES to point at it.
PRIOR = Path(os.environ.get("CODVR_PRIOR_TEXTURES", ""))
GAME_MAIN = Path(r"H:\SteamLibrary\steamapps\common\Call of Duty\Main")
EXPORTER = Path(r"C:\Program Files\NVIDIA Corporation\RTX Remix\exts\lightspeed.trex.app.resources\deps\tools\nvtt\nvtt_export.exe")
MATERIALS = ("brick", "plaster", "wood", "metal", "stone", "cobble", "soil", "cloth", "concrete", "bark", "grass", "rock")
IMAGE_EXTENSIONS = {".dds", ".tga", ".jpg", ".jpeg", ".png"}
IDENTITY = re.compile(r"(?:face|head|flesh|teeth|mouth|eye|hair|viewhands|viewhand|hands?(?:[._@/]|$))", re.I)
EFFECT = re.compile(r"(?:smoke|explosion|muzzle|flare|corona|sprite|beam|firetexture|flame|particle|glow|(?:lamp\d*|light\d*[ab]?|bulb)_on)", re.I)
PRESERVE_CATEGORIES = {
    "HUD / menu / UI / fonts": "faithful_ui_preservation",
    "signs / labels / text-containing textures": "faithful_text_preservation",
    "special-purpose technical textures": "faithful_technical_preservation",
    "skyboxes": "faithful_sky_set_preservation",
    "uncertain / manual-review assets": "faithful_ambiguous_preservation",
}
FAITHFUL = "faithful_"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(8 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def logical_name(path: str) -> str:
    return str(Path(path.replace("\\", "/").lower()).with_suffix("")).replace("\\", "/")


def truth(value) -> bool:
    return str(value).lower() in {"true", "1", "yes"}


def load_rows(path: Path) -> list[dict]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    logical = [logical_name(row["asset_path"]) for row in rows]
    if len(logical) != len(set(logical)):
        raise ValueError("Source manifest has duplicate logical texture names")
    for row in rows:
        for field in ("source_width", "source_height", "target_width", "target_height"):
            row[field] = int(row[field])
        for field in ("source_alpha", "likely_tile", "contains_text"):
            row[field] = truth(row[field])
        if min(row["source_width"], row["source_height"]) < 1:
            raise ValueError(f"Invalid source size for {row['id']}")
    return rows


def winning_paths(main: Path) -> dict[str, list[dict]]:
    """Record physical winners; logical aliases are recorded, not guessed away."""
    winners = {}
    for pack in sorted(main.glob("*.pk3"), key=lambda p: p.name.lower()):
        with zipfile.ZipFile(pack) as archive:
            for info in archive.infolist():
                name = info.filename.replace("\\", "/")
                if Path(name).suffix.lower() in IMAGE_EXTENSIONS:
                    winners[name.lower()] = {"archive": str(pack), "entry": name,
                                             "bytes": info.file_size, "crc32": info.CRC}
    by_logical = collections.defaultdict(list)
    # Header-only metadata reads do not decode, resize, or recompress artwork.
    # Keep every physical alias: explicit TGA/JPG references must remain valid.
    with contextlib.ExitStack() as stack:
        opened = {}
        for name, source in sorted(winners.items()):
            path = source["archive"]
            if path not in opened:
                opened[path] = stack.enter_context(zipfile.ZipFile(path))
            suffix = Path(source["entry"]).suffix.lower()
            with opened[path].open(source["entry"]) as stream:
                header = stream.read(262144 if suffix in {".jpg", ".jpeg"} else 128)
            width, height = image_header_dimensions(header, suffix)
            source.update(width=width, height=height)
            by_logical[logical_name(name)].append(source)
    return dict(by_logical)


def image_header_dimensions(data: bytes, suffix: str) -> tuple[int, int]:
    """Read dimensions only; never turn preserved content into a new raster."""
    width = height = 0
    if suffix == ".dds" and len(data) >= 20 and data[:4] == b"DDS ":
        height, width = struct.unpack_from("<2I", data, 12)
    elif suffix == ".tga" and len(data) >= 18:
        width, height = struct.unpack_from("<2H", data, 12)
    elif suffix == ".png" and len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n":
        width, height = struct.unpack_from(">2I", data, 16)
    elif suffix in {".jpg", ".jpeg"} and data[:2] == b"\xff\xd8":
        index = 2
        while index+4 <= len(data):
            if data[index] != 255:
                break
            while index < len(data) and data[index] == 255:
                index += 1
            if index >= len(data):
                break
            marker = data[index]
            index += 1
            if marker in {0xD8, 0xD9, 0x01} or 0xD0 <= marker <= 0xD7:
                continue
            if index+2 > len(data):
                break
            length = struct.unpack_from(">H", data, index)[0]
            if length < 2 or index+length > len(data):
                break
            if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
                height, width = struct.unpack_from(">2H", data, index+3)
                break
            index += length
    if min(width, height) < 1:
        raise ValueError(f"Cannot read preserved image dimensions from {suffix} header")
    return width, height


def source_coverage_audit(rows: list[dict], winners: dict) -> dict:
    planned = {logical_name(row["asset_path"]) for row in rows}
    active = set(winners)
    extra, missing = sorted(active-planned), sorted(planned-active)
    return {"manifest_logical_assets": len(planned), "active_logical_assets": len(active),
            "active_physical_aliases": sum(len(entries) for entries in winners.values()),
            "active_logical_not_in_manifest": extra, "manifest_without_active_winner": missing,
            "complete_logical_coverage": not extra and not missing,
            "source_scope": "PK3 runtime image resources; loose savegame screenshots are not rendering assets"}


def material_for(row: dict) -> str:
    name = row["asset_path"].lower()
    if row["category"] == "character / uniform / skin textures" and any(word in name for word in ("boot", "jerkin")):
        return "leather"
    if any(word in name for word in ("dualwire", "105mm", "38cm_shell", "38cm_fuze")):
        return "metal"
    if "parachute" in name or "eureka_bag" in name:
        return "cloth"
    if "statue" in name:
        return "stone"
    if any(word in name for word in ("cobble", "cobbleroad")):
        return "cobble"
    if any(word in name for word in ("bark", "treebark", "treetrunk", "tree_trunk")):
        return "bark"
    if any(word in name for word in ("foliage", "leaf", "leaves", "bush", "grass", "treeline")):
        return "grass"
    if any(word in name for word in ("asphalt", "road")) and row["material"] == "terrain":
        return "concrete"
    if any(word in name for word in ("rubble", "rock", "cliff")):
        return "rock"
    return {"terrain": "soil", "generic_architecture": "stone",
            "weapon": "metal"}.get(row["material"], row["material"])


def route_for(row: dict) -> tuple[str, str]:
    category, name = row["category"], row["asset_path"].lower()
    if any(value & (value-1) for value in (row["source_width"], row["source_height"])):
        return "faithful_engine_format_preservation", "Engine DDS loader rejects non-power-of-two sizes; preserve current format/aspect"
    if category in PRESERVE_CATEGORIES:
        return PRESERVE_CATEGORIES[category], "Functional artwork/meaning requires dedicated authoring; retained honestly"
    if row["contains_text"]:
        return "faithful_marking_preservation", "Lettering, insignia, gauges and labels retain source semantics"
    if IDENTITY.search(name):
        return "faithful_identity_preservation", "Facial/hand identity atlas requires individual art/model review"
    if EFFECT.search(name):
        return "faithful_translucent_effect_preservation", "Preserve animated/additive effect response and alpha"
    if row["material"] == "glass" or "glass" in name or "mirror" in name:
        return "faithful_glass_preservation", "Preserve reflection/transmission artwork and shader assumptions"
    if min(row["source_width"], row["source_height"]) <= 16:
        return "faithful_small_signal_preservation", "Small texture may encode a constant/control signal"
    if category == "decals":
        return "faithful_decal_preservation", "Decal shape/message retained; bespoke repaint is not yet authored"
    assigned = material_for(row)
    if assigned == "leather":
        return "faithful_unavailable_material_preservation", "Leather donor is not authored; do not substitute cloth weave/wood grain"
    if assigned not in MATERIALS and category not in {"weapon textures", "character / uniform / skin textures"}:
        return "faithful_unclassified_preservation", "No verified material assignment or suitable authored donor"
    if category == "foliage / vegetation / alpha-cutout assets":
        return "alpha_material_reinterpretation", "New botanical surface detail inside the original coverage mask"
    if row["source_alpha"]:
        return "alpha_atlas_material_reinterpretation", "Protected layout and exact master alpha with new surface material"
    if category == "architecture / walls / floors / terrain" and row["likely_tile"]:
        if row["material"] in {"generic_architecture", "unknown"}:
            return "faithful_unclassified_preservation", "No verified surface-material classification"
        return "world_material_reinterpretation", "Coherent new material surface, source macro weathering and original coordinate scale"
    if category == "weapon textures":
        return "weapon_atlas_material_reinterpretation", "Separate stock/wood/metal masks; source layout, sights and edges protected"
    if category == "character / uniform / skin textures":
        return "character_atlas_material_reinterpretation", "Period cloth with separate leather/metal masks and protected patches/seams"
    if category == "props" and row["material"] in {"generic_prop", "unknown"}:
        return "faithful_unclassified_preservation", "Unknown mixed prop must not be repainted as generic metal"
    if material_for(row) not in MATERIALS:
        return "faithful_unclassified_preservation", "No suitable authored donor or material assignment"
    return "structural_material_reinterpretation", "New material artwork within protected panel/atlas boundaries"


def target_size(row: dict, route: str, cap: int = 2048) -> tuple[int, int]:
    w, h = row["source_width"], row["source_height"]
    if route.startswith(FAITHFUL):
        return w, h
    requested = max(row["target_width"] / w, row["target_height"] / h)
    # Integer/power-of-two scale preserves authored aspect and old engine sizes.
    scale = max(1, min(4, int(requested)))
    while max(w, h) * scale > cap and scale > 1:
        scale //= 2
    return w * scale, h * scale


def plan_row(row: dict, winners: dict, cap: int) -> dict:
    route, reason = route_for(row)
    size = target_size(row, route, cap)
    output = logical_name(row["asset_path"]) + ".dds"
    current = winners.get(logical_name(row["asset_path"]), [])
    planned = {
        **row, "route": route, "route_reason": reason, "assigned_material": material_for(row),
        "output_asset_path": output, "target_width": size[0], "target_height": size[1],
        "winning_sources": current, "output_strategy": "new_dds_from_stock_master",
        "coverage": "faithful_preservation" if route.startswith(FAITHFUL) else "material_reinterpretation_planned",
        "uv_validation": "source_layout_and_edges; rendered_model_review_required" if "atlas" in route else "not_an_atlas_route",
    }
    if route.startswith(FAITHFUL):
        if not current:
            raise ValueError(f"No current winning bytes for preservation: {row['asset_path']}")
        primary = ([entry for entry in current if entry["entry"].lower().endswith(".dds")] or current)[-1]
        art_size = target_size(row, "material_reinterpretation", cap)
        planned.update(output_strategy="current_winning_bytes", output_asset_path=None,
                       target_width=primary["width"], target_height=primary["height"],
                       reinterpret_target_width=art_size[0], reinterpret_target_height=art_size[1],
                       output_entries=[{**entry, "output_asset_path": entry["entry"]} for entry in current],
                       preservation_policy="Every current winning physical alias byte-identical; no stock downgrade or recompression",
                       uv_validation="current_winning_bytes_unchanged")
    return planned


def coverage_report(records: list[dict], *, complete: bool) -> dict:
    passing = [rec for rec in records if rec.get("validation") == "PASS"]
    changed = [rec for rec in passing if rec.get("artistic_change") == "material_reinterpreted"]
    preserved = [rec for rec in records if rec["route"].startswith(FAITHFUL)]
    return {
        "pipeline_version": VERSION, "logical_assets": len(records), "complete": complete,
        "routes": dict(collections.Counter(rec["route"] for rec in records)),
        "categories": dict(collections.Counter(rec["category"] for rec in records)),
        "faithfully_preserved": len(preserved), "material_reinterpreted_and_static_passed": len(changed),
        "current_winning_bytes_preserved_and_hash_passed": sum(rec.get("artistic_change") == "current_winning_bytes_preserved"
                                                               and rec.get("validation") == "PASS" for rec in preserved),
        "preserved_physical_aliases_planned": sum(len(rec.get("output_entries", [])) for rec in preserved),
        "not_yet_processed": sum(rec.get("validation") not in {"PASS", "FAIL"} for rec in records),
        "failed": sum(rec.get("validation") == "FAIL" for rec in records),
        "zero_change_not_reinterpreted": sum(rec.get("artistic_change") == "no_effective_change" for rec in records),
        "unclassified_requires_bespoke_art": sum(rec["route"] == "faithful_unclassified_preservation" for rec in records),
        "all_textures_reinterpreted": len(changed) == len(records) and complete,
        "visual_validation": "Representative contact sheets are produced; no claim of human or in-game approval",
        "scope": "Diffuse material artwork only; no game models, UVs, shaders, weapon stats, VR input or live installation changes",
    }


def dds_audit(data: bytes) -> dict:
    if len(data) < 128 or data[:4] != b"DDS " or struct.unpack_from("<I", data, 4)[0] != 124:
        raise ValueError("Not a legacy DDS header")
    height, width = struct.unpack_from("<2I", data, 12)
    count = struct.unpack_from("<I", data, 28)[0] or 1
    fourcc = data[84:88]
    block = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}.get(fourcc)
    if not block or min(width, height) < 1:
        raise ValueError("Unsupported DDS format/dimensions")
    expected_count = max(width, height).bit_length()
    if count != expected_count:
        raise ValueError(f"Incomplete/excessive mip chain: {count}, expected {expected_count}")
    offset, levels = 128, []
    for level in range(count):
        w, h = max(1, width >> level), max(1, height >> level)
        size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
        levels.append({"width": w, "height": h, "offset": offset, "bytes": size})
        offset += size
    if offset != len(data):
        raise ValueError(f"DDS payload length {len(data)} does not match header/mips {offset}")
    return {"width": width, "height": height, "fourcc": fourcc.decode(), "mips": count,
            "ends_at_1x1": levels[-1]["width"] == levels[-1]["height"] == 1,
            "levels": levels, "trailing_bytes": 0}


def alpha_coverage_error(expected, actual) -> dict:
    import numpy as np
    a, b = np.asarray(expected, dtype=np.float32), np.asarray(actual, dtype=np.float32)
    return {"mean_absolute": float(np.abs(a - b).mean() / 255),
            "coverage_error_128": float(abs((a >= 128).mean() - (b >= 128).mean())),
            "coverage_error_16": float(abs((a >= 16).mean() - (b >= 16).mean()))}


def alpha_validation_errors(metrics: dict, binary: bool) -> list[str]:
    errors = []
    if metrics["mean_absolute"] > (0.018 if binary else 0.025):
        errors.append("dds_alpha_error")
    if metrics["coverage_error_128"] > 0.005:
        errors.append("dds_alpha_coverage")
    if not binary and metrics["coverage_error_16"] > 0.01:
        errors.append("dds_faint_alpha_coverage")
    return errors


def is_cutout(row: dict, fmt: str) -> bool:
    return fmt == "bc1a" or row["category"] == "foliage / vegetation / alpha-cutout assets"


def mip_alpha_validation_errors(level: dict, cutout: bool) -> list[str]:
    # At 1x1 an arbitrary coverage fraction cannot be represented by binary
    # alpha. Allow one texel of quantization, not an unchecked small-mip pass.
    texels = level["width"] * level["height"]
    tolerance = max(.015, 1/texels)
    if cutout:
        return ["cutout_mip_coverage"] if level["coverage_error_128"] > tolerance else []
    return ["soft_mip_mean_alpha"] if level["mean_alpha_error"] > .04 else []


def decode_dds_level(data: bytes, level: dict):
    import io
    from PIL import Image
    header = bytearray(data[:128])
    struct.pack_into("<2I", header, 12, level["height"], level["width"])
    struct.pack_into("<I", header, 20, level["bytes"])
    struct.pack_into("<I", header, 28, 1)
    image = Image.open(io.BytesIO(bytes(header)+data[level["offset"]:level["offset"]+level["bytes"]]))
    image.load()
    return image.convert("RGBA")


def audit_mip_alpha(data: bytes, expected_alpha, *, cutout: bool) -> dict:
    import numpy as np
    from PIL import Image
    alpha = np.asarray(expected_alpha, dtype=np.float32)/255
    baseline_coverage = float((alpha >= 128/255).mean())
    records, errors = [], []
    for index, level in enumerate(dds_audit(data)["levels"]):
        decoded = decode_dds_level(data, level)
        got = np.asarray(decoded.getchannel("A"), dtype=np.float32)/255
        box = np.asarray(expected_alpha.resize((level["width"], level["height"]), Image.Resampling.BOX), dtype=np.float32)/255
        record = {"level": index, "width": level["width"], "height": level["height"],
                  "coverage_128": float((got >= 128/255).mean()),
                  "coverage_error_128": float(abs((got >= 128/255).mean()-baseline_coverage)),
                  "mean_alpha": float(got.mean()), "mean_alpha_error": float(abs(got.mean()-box.mean())),
                  "coverage_tolerance": max(.015, 1/got.size)}
        record["errors"] = mip_alpha_validation_errors(record, cutout)
        errors.extend(f"mip_{index}:{error}" for error in record["errors"])
        records.append(record)
    return {"cutout": cutout, "source_coverage_128": baseline_coverage, "levels": records, "errors": errors,
            "scope": "Every mip decoded; cutout cutoff coverage with one-texel allowance, or soft opacity mean; not pixel-exact filtering"}


def override_metadata_errors(entry: dict, digest: str, source_digest: str) -> list[str]:
    errors = []
    if not entry.get("prompt") or not entry.get("origin"):
        errors.append("missing_override_generation_provenance")
    if entry.get("preserve_layout") is not True:
        errors.append("override_must_declare_source_locked_layout")
    if entry.get("sha256") and entry["sha256"].lower() != digest:
        errors.append("override_digest_mismatch")
    if entry.get("source_png_sha256") and entry["source_png_sha256"].lower() != source_digest:
        errors.append("override_source_digest_mismatch")
    return errors


def attach_art_inputs(plans: list[dict], overrides: Path, masks: Path, provenance_path: Path) -> None:
    entries = {}
    if provenance_path.exists():
        content = json.loads(provenance_path.read_text(encoding="utf-8"))
        entries = content.get("overrides", content)
    for row in plans:
        path = overrides/f"{row['id']}.png"
        if path.exists():
            row["override"] = {**entries.get(row["id"], {}), "file": str(path), "sha256_actual": sha256(path)}
            row["original_route"] = row["route"]
            row["route"] = "bespoke_source_locked_reinterpretation"
            row["route_reason"] = "Individual AI-authored repaint; source layout/alpha/feature guards and visual review required"
            row["coverage"] = "bespoke_reinterpretation_planned"
            if row.get("output_strategy") == "current_winning_bytes":
                row.update(output_strategy="new_dds_from_stock_master",
                           output_asset_path=logical_name(row["asset_path"])+".dds",
                           target_width=row["reinterpret_target_width"], target_height=row["reinterpret_target_height"])
                row.pop("output_entries", None)
                row.pop("preservation_policy", None)
                row["uv_validation"] = "source_layout_and_edges; rendered_model_review_required"
        mask = masks/f"{row['id']}.png"
        if mask.exists():
            row["protection_mask"] = {"file": str(mask), "sha256": sha256(mask),
                                       "meaning": "White protects original pixels; black permits repaint"}


def load_materials(folder: Path, provenance_path: Path) -> tuple[dict, dict]:
    from PIL import Image
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    entries = provenance.get("materials", provenance)
    images, receipts = {}, {}
    for material in MATERIALS:
        path = folder / f"{material}.png"
        entry = entries.get(material, {})
        if not path.exists() or not entry.get("prompt") or not entry.get("origin"):
            raise ValueError(f"Missing authored image/provenance for material {material}")
        digest = sha256(path)
        if entry.get("sha256") and entry["sha256"].lower() != digest:
            raise ValueError(f"Material provenance digest mismatch for {material}")
        with Image.open(path) as image:
            image.load()
            if min(image.size) < 512:
                raise ValueError(f"Material {material} is too small")
            images[material] = image.convert("RGB")
        receipts[material] = {**entry, "file": str(path), "sha256": digest}
    return images, receipts


def resize_source(source, size):
    """Straight-alpha result produced through float premultiplied filtering."""
    import numpy as np
    from PIL import Image
    if source.size == size:
        return source.copy()
    if "A" not in source.getbands():
        return source.convert("RGB").resize(size, Image.Resampling.LANCZOS)
    rgba = np.asarray(source.convert("RGBA"), dtype=np.float32) / 255
    alpha = rgba[:, :, 3]
    channels = [np.asarray(Image.fromarray(rgba[:, :, c] * alpha).resize(size, Image.Resampling.LANCZOS)) for c in range(3)]
    resized_alpha = source.getchannel("A").resize(size, Image.Resampling.LANCZOS)
    a = np.asarray(resized_alpha, dtype=np.float32) / 255
    rgb = np.stack(channels, axis=2) / np.maximum(a[:, :, None], 1 / 255)
    output = Image.fromarray(np.clip(np.round(rgb * 255), 0, 255).astype("uint8"), "RGB")
    output.putalpha(resized_alpha)
    return output


def periodic_material(image, size, *, source_scale: float, material_scale: float, seed: int):
    """One scale for both axes; rectangular targets receive a tiled crop."""
    import numpy as np
    from PIL import Image
    # A donor repeat corresponds to 256 source texels, adjusted by the donor's
    # measured pattern period when a repeat estimate is available.
    repeat = max(32, round(256 * source_scale * material_scale))
    factor = repeat / image.width
    sampled = image.resize((repeat, max(1, round(image.height * factor))), Image.Resampling.LANCZOS)
    arr = np.asarray(sampled, dtype=np.float32) / 255
    h, w = arr.shape[:2]
    # Feather tile boundaries before wrap. No independent X/Y resizing.
    band = max(2, min(h, w) // 24)
    for i in range(band):
        t = (i + 1) / (band + 1)
        pair = (arr[:, i] + arr[:, -1 - i]) * .5
        arr[:, i] = pair * (1 - t) + arr[:, i] * t
        arr[:, -1 - i] = pair * (1 - t) + arr[:, -1 - i] * t
        pair = (arr[i] + arr[-1 - i]) * .5
        arr[i] = pair * (1 - t) + arr[i] * t
        arr[-1 - i] = pair * (1 - t) + arr[-1 - i] * t
    yy = (np.arange(size[1]) + seed * 43) % h
    xx = (np.arange(size[0]) + seed * 67) % w
    return arr[yy[:, None], xx[None, :]]


def dominant_repeat(image) -> float | None:
    """Estimate a repeated material period; no claim of real-world metres."""
    import numpy as np
    from scipy.ndimage import gaussian_filter
    gray = np.asarray(image.convert("L"), dtype=np.float32)
    periods = []
    for axis in (0, 1):
        profile = gray.mean(axis=axis)
        if len(profile) < 64:
            continue
        profile -= gaussian_filter(profile, max(2, len(profile) / 24))
        spectrum = np.fft.rfft(profile - profile.mean())
        correlation = np.fft.irfft(spectrum * spectrum.conjugate(), n=len(profile))
        if correlation[0] <= 1e-6:
            continue
        lo, hi = max(8, len(profile) // 32), len(profile) // 3
        peaks = [i for i in range(lo + 1, hi - 1) if correlation[i] > correlation[i-1]
                 and correlation[i] > correlation[i+1] and correlation[i] / correlation[0] > .25]
        if peaks:
            periods.append(float(max(peaks, key=lambda i: correlation[i])))
    return sum(periods) / len(periods) if periods else None


def close_target_seams(array):
    """Match opposite target edges after an isotropic donor crop, without warping."""
    import numpy as np
    output = array.copy()
    height, width = output.shape[:2]
    band = max(2, min(height, width)//24)
    for index in range(band):
        weight = .5*(1-math.cos(math.pi*index/max(1, band-1)))
        pair = (output[:, index]+output[:, -1-index])*.5
        output[:, index] = pair*(1-weight)+output[:, index]*weight
        output[:, -1-index] = pair*(1-weight)+output[:, -1-index]*weight
        pair = (output[index]+output[-1-index])*.5
        output[index] = pair*(1-weight)+output[index]*weight
        output[-1-index] = pair*(1-weight)+output[-1-index]*weight
    return np.clip(output, 0, 1)


def structure_protection(base):
    import numpy as np
    from scipy.ndimage import gaussian_filter, maximum_filter, sobel
    y = .2126 * base[:, :, 0] + .7152 * base[:, :, 1] + .0722 * base[:, :, 2]
    smooth = gaussian_filter(y, 1.25)
    edge = np.hypot(sobel(smooth, 0), sobel(smooth, 1))
    threshold = max(.12, float(np.percentile(edge, 83)))
    protected = edge > threshold
    protected = maximum_filter(protected.astype(float), size=5) > .5
    # Low/high-value unpainted atlas padding and saturated badges are protected.
    padding = (base.max(2) - base.min(2) < .012) & ((y < .025) | (y > .975))
    protected |= padding
    return protected, y


def source_semantic_protection(row, source, size):
    """Guard stock-resolution compact features, not a claim of OCR/UV proof."""
    import numpy as np
    from PIL import Image
    from scipy.ndimage import gaussian_filter, maximum_filter
    rgb = np.asarray(source.convert("RGB"), dtype=np.float32)/255
    y = .2126*rgb[:, :, 0]+.7152*rgb[:, :, 1]+.0722*rgb[:, :, 2]
    # Detect muted compact details at original texel scale, before enlargement
    # spreads their contrast below a full-resolution gradient threshold.
    compact = np.abs(y-gaussian_filter(y, max(1.5, min(source.size)/96))) > .026
    protected = maximum_filter(compact.astype(float), size=3) > .5
    if row["category"] == "character / uniform / skin textures":
        r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
        warm = np.clip((r-b-.025)/.16, 0, 1)*np.clip((r-g+.01)/.10, 0, 1)
        # Confident leather islands are binary source locks. No residual cloth
        # weight is allowed on this mask, unlike the earlier soft complement.
        protected |= (warm >= .45) & (y < .48) & (y > .025)
    mask = Image.fromarray(protected.astype("uint8")*255).resize(size, Image.Resampling.NEAREST)
    result = np.asarray(mask) >= 128
    if row.get("protection_mask"):
        with Image.open(row["protection_mask"]["file"]) as supplied:
            supplied.load()
            if supplied.width*source.height != supplied.height*source.width:
                raise ValueError("Explicit semantic/UV protection mask has wrong aspect ratio")
            explicit = supplied.convert("L").resize(size, Image.Resampling.NEAREST)
            result |= np.asarray(explicit) >= 128
    return result


def material_blend_guard(protected, scale):
    import numpy as np
    from scipy.ndimage import distance_transform_edt
    if not protected.any():
        return np.ones(protected.shape, dtype=float)
    distance = distance_transform_edt(~protected)
    weight = np.clip(distance/max(1, 2*scale), 0, 1)
    return weight*weight*(3-2*weight)


def material_masks(row, base):
    """Explicit named-part overrides plus mixed-atlas material segmentation."""
    import numpy as np
    from scipy.ndimage import gaussian_filter
    name, route = row["asset_path"].lower(), row["route"]
    r, g, b = base[:, :, 0], base[:, :, 1], base[:, :, 2]
    y = .2126*r + .7152*g + .0722*b
    saturation = base.max(2) - base.min(2)
    warm = np.clip((r-b-.025) / .16, 0, 1) * np.clip((r-g+.01) / .10, 0, 1)
    warm *= np.clip((y-.035) / .14, 0, 1)
    neutral = np.clip(1 - saturation / .24, 0, 1)
    one = np.ones_like(y)
    if row["category"] == "weapon textures":
        wood_part = any(word in name for word in ("stock", "weapon_m1gstock", "enfieldstock", "ppshstock"))
        metal_part = any(word in name for word in ("reciever", "receiver", "barrel", "sight", "scope", "site"))
        no_wood = any(word in name for word in ("colt", "luger", "mp40", "sten", "grenade", "panzerfaust", "mg42"))
        if wood_part:
            masks = {"wood": one}
        elif metal_part:
            masks = {"metal": one}
        elif no_wood:
            # Dark polymer/leather grips keep their original structure and do
            # not acquire random wood grain; only neutral metal is reauthored.
            masks = {"metal": neutral}
        else:
            masks = {"wood": warm, "metal": (1-warm) * neutral}
    elif row["category"] == "character / uniform / skin textures" and row["assigned_material"] == "metal":
        masks = {"metal": one}
    elif row["category"] == "character / uniform / skin textures":
        leather = warm * np.clip((.48-y) / .30, 0, 1)
        metal = neutral * np.clip((y-.60) / .25, 0, 1)
        # No leather donor is authored in this library. Protect leather rather
        # than imprint wood grain or cloth weave on belts/boots/holsters.
        masks = {"cloth": np.clip(1-leather-metal, 0, 1), "metal": metal}
    elif row["category"] == "foliage / vegetation / alpha-cutout assets" and row["assigned_material"] == "grass":
        # Meadow detail must not paint grass onto a tree trunk or branches.
        green = np.clip((g-r-.015)/.065, 0, 1)*np.clip((g-b-.015)/.065, 0, 1)
        masks = {"grass": green}
    else:
        material = row["assigned_material"]
        if material not in MATERIALS:
            raise ValueError(f"No donor for planned reinterpretation {row['id']}: {material}")
        masks = {material: one}
    return {key: np.clip(gaussian_filter(mask, .75), 0, 1) for key, mask in masks.items()}


def render(row, source, materials):
    import numpy as np
    from PIL import Image
    from scipy.ndimage import gaussian_filter
    size = row["target_width"], row["target_height"]
    resized = resize_source(source, size)
    if row["route"].startswith(FAITHFUL):
        return resized, {"donors": [], "protected_fraction": 1.0,
                         "material_scale_method": "no_new_material", "source_edge_delta": 0.0}
    base = np.asarray(resized.convert("RGB"), dtype=np.float32) / 255
    scale = size[0] / source.width
    seed = int(hashlib.sha256(row["asset_path"].encode()).hexdigest()[:8], 16)
    protected, y = structure_protection(base)
    protected |= source_semantic_protection(row, source, size)
    blend_guard = material_blend_guard(protected, scale)
    if row.get("override"):
        override = row["override"]
        errors = override_metadata_errors(override, override["sha256_actual"], sha256(Path(row["source_png"])))
        if errors:
            raise ValueError(f"{row['id']}: {errors}")
        with Image.open(override["file"]) as authored:
            authored.load()
            if authored.width*source.height != authored.height*source.width:
                raise ValueError("Bespoke override changes source UV aspect ratio; never stretch it to fit")
            authored = resize_source(authored, size)
            if "A" in authored.getbands() and authored.getchannel("A").getextrema() != (255, 255):
                expected = resized.getchannel("A") if "A" in resized.getbands() else Image.new("L", size, 255)
                if not np.array_equal(np.asarray(authored.getchannel("A")), np.asarray(expected)):
                    raise ValueError("Bespoke override changes source alpha coverage")
            output = np.asarray(authored.convert("RGB"), dtype=np.float32)/255
        output[protected] = base[protected]
        result = Image.fromarray(np.round(output*255).astype("uint8"), "RGB")
        if "A" in resized.getbands():
            result.putalpha(resized.getchannel("A"))
        return result, {"donors": [], "bespoke_override": override, "protected_fraction": float(protected.mean()),
                        "material_scale_method": "individual_source_locked_repaint; isotropic_resize_only",
                        "source_edge_delta": 0.0, "semantic_validation": "source compact-feature locks plus optional explicit masks; model/marking review pending",
                        "master_alpha_policy": "Stock alpha restored; supplied nonopaque override alpha must match exactly"}
    assigned = row["assigned_material"]
    world = row["route"] == "world_material_reinterpretation"
    masks = {assigned: np.ones_like(y)} if world else material_masks(row, base)
    output = base.copy()
    material_scales = {}
    for index, (material, mask) in enumerate(masks.items()):
        material_scale = 1.0
        if world and material in {"brick", "cobble", "wood", "stone"}:
            source_period, donor_period = dominant_repeat(source), dominant_repeat(materials[material])
            if source_period and donor_period:
                material_scale = float(np.clip(source_period / (donor_period * 256/materials[material].width), .5, 2))
        material_scales[material] = material_scale
        donor = periodic_material(materials[material], size, source_scale=scale,
                                  material_scale=material_scale, seed=seed + index)
        donor_y = .2126*donor[:, :, 0] + .7152*donor[:, :, 1] + .0722*donor[:, :, 2]
        if world or row["category"] == "character / uniform / skin textures" or material == "cloth":
            # Source owns joints, stains, uniform patterns and period palette.
            # Only fine donor grain is admitted; macro motifs/twill previously
            # introduced competing joints or visible fabric-shaped patches.
            palette = base.mean(axis=(0, 1))/np.maximum(donor.mean(axis=(0, 1)), .025)
            donor = np.clip(donor*palette[None, None, :], 0, 1)
            fine = donor-gaussian_filter(donor, max(1, 1.3*scale), axes=(0, 1))
            grain = .70*np.clip(fine, -.07, .07)
            guard = blend_guard if world else blend_guard*mask
            output = np.clip(output+grain*guard[:, :, None], 0, 1)
        else:
            # Keep macro atlas partitions but repaint material inside them.
            # Donor structure and color contribute well above the old ~3% noise
            # graft, while silhouettes/engravings/panel seams remain protected.
            low_donor = gaussian_filter(donor, max(3, 6*scale), axes=(0, 1))
            detail = donor-low_donor
            broad_base = gaussian_filter(base, max(2, 5*scale), axes=(0, 1))
            shaded = donor * np.clip((y+.08) / (donor_y+.08), .45, 1.5)[:, :, None]
            candidate = .65*base + .22*shaded + .36*detail + .13*(broad_base-base)
            guard = blend_guard * mask
            # Strongly colored badges/paint markings remain source-authored.
            saturation = base.max(2)-base.min(2)
            guard *= np.clip(1 - np.maximum(saturation-.38, 0) / .20, 0, 1)
            output += (np.clip(candidate, 0, 1)-base) * guard[:, :, None]
    if world:
        output = close_target_seams(output)
    else:
        output[protected] = base[protected]
    result = Image.fromarray(np.round(np.clip(output, 0, 1)*255).astype("uint8"), "RGB")
    if "A" in resized.getbands():
        result.putalpha(resized.getchannel("A"))
    delta = np.abs(output-base)
    edge_delta = float(delta[protected].max()) if protected.any() and not world else None
    return result, {"donors": list(masks), "protected_fraction": float(protected.mean()) if not world else 0.0,
                    "material_scales": material_scales, "material_scale_method": "isotropic_period_match_or_256_source_texel_repeat",
                    "physical_scale_limitation": "Original UV coordinates retained; actual metres/texel need in-game review",
                    "preserved_material_regions": ["high_confidence_leather_mask"] if row["category"] == "character / uniform / skin textures" else [],
                    "semantic_validation": "Source features/provided masks protected; not a guarantee of all muted insignia or actual UV islands",
                    "source_edge_delta": edge_delta}


def change_metrics(source, output) -> dict:
    import numpy as np
    from PIL import Image
    from scipy.ndimage import gaussian_filter
    size = source.size
    a = np.asarray(source.convert("RGB"), dtype=np.float32) / 255
    b = np.asarray(output.convert("RGB").resize(size, Image.Resampling.LANCZOS), dtype=np.float32) / 255
    y1 = .2126*a[:, :, 0]+.7152*a[:, :, 1]+.0722*a[:, :, 2]
    y2 = .2126*b[:, :, 0]+.7152*b[:, :, 1]+.0722*b[:, :, 2]
    difference = np.abs(a-b)
    visible = np.asarray(source.getchannel("A")) >= 16 if "A" in source.getbands() else np.ones(y1.shape, dtype=bool)
    if not visible.any():
        visible = np.zeros(y1.shape, dtype=bool)
    low1, low2 = gaussian_filter(y1, 3), gaussian_filter(y2, 3)
    correlation = float(np.corrcoef(low1.ravel(), low2.ravel())[0, 1]) if min(low1.std(), low2.std()) > 1e-6 else None
    edge_error = float((np.abs(b[:, 0]-b[:, -1]).mean()+np.abs(b[0]-b[-1]).mean())*.5)
    return {"mean_absolute_rgb_change": float(difference[visible].mean()) if visible.any() else 0.0,
            "pixels_changed_over_5_255": float((difference.max(2)[visible] > 5/255).mean()) if visible.any() else 0.0,
            "visible_source_fraction": float(visible.mean()),
            "macro_luminance_correlation": correlation,
            "mean_luminance_delta": float(y2[visible].mean()-y1[visible].mean()) if visible.any() else 0.0,
            "clipped_fraction": float(((b <= 1/255) | (b >= 254/255)).all(2)[visible].mean()) if visible.any() else 0.0,
            "target_wrap_edge_error_at_stock_scale": edge_error}


def record_fingerprint(row, material_receipts: dict) -> str:
    content = {"version": VERSION, "implementation_sha256": sha256(Path(__file__)), "row": row,
               "donor_hashes": {key: value["sha256"] for key, value in material_receipts.items()}}
    return hashlib.sha256(json.dumps(content, sort_keys=True).encode()).hexdigest()


def current_entry_bytes(entry: dict) -> bytes:
    with zipfile.ZipFile(entry["archive"]) as archive:
        info = archive.getinfo(entry["entry"])
        if (entry.get("bytes", info.file_size) != info.file_size
                or entry.get("crc32", info.CRC) != info.CRC):
            raise ValueError(f"Current winning source changed since plan: {entry['entry']}")
        data = archive.read(info)
    if entry.get("sha256") and hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise ValueError(f"Current winning source hash changed: {entry['entry']}")
    return data


def candidate_preservation_plan(row: dict, errors: list[str]) -> dict:
    allowed = {"insufficient_artistic_change", "dds_alpha_error", "dds_alpha_coverage", "dds_faint_alpha_coverage"}
    if not errors or any(error not in allowed and not re.fullmatch(r"mip_\d+:(cutout_mip_coverage|soft_mip_mean_alpha)", error)
                         for error in errors):
        raise ValueError("Only ineffective or alpha-unsafe artwork may use current-byte preservation")
    current = row["winning_sources"]
    if not current:
        raise ValueError("Rejected candidate has no safe current-byte fallback")
    primary = ([entry for entry in current if entry["entry"].lower().endswith(".dds")] or current)[-1]
    return {**row, "route": "faithful_rejected_candidate_preservation", "original_route": row["route"],
            "route_reason": "Candidate rejected, not installed: "+", ".join(errors),
            "output_strategy": "current_winning_bytes", "output_asset_path": None,
            "target_width": primary["width"], "target_height": primary["height"],
            "output_entries": [{**entry, "output_asset_path": entry["entry"]} for entry in current],
            "uv_validation": "current_winning_bytes_unchanged"}


def process(row, output_dir, materials, receipts, exporter):
    if row["route"].startswith(FAITHFUL) and row.get("output_strategy") != "current_winning_bytes":
        raise ValueError("Preservation requires planned current winning bytes; stock raster export is forbidden")
    if row.get("output_strategy") == "current_winning_bytes":
        entries = []
        for entry in row["output_entries"]:
            data = current_entry_bytes(entry)
            dimensions = image_header_dimensions(data, Path(entry["entry"]).suffix.lower())
            if dimensions != (entry["width"], entry["height"]):
                raise ValueError(f"Current winning dimensions changed: {entry['entry']}")
            entries.append({**entry, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        record = {**row, "output_entries": entries, "fingerprint": record_fingerprint(row, {}),
                  "donors": [], "artistic_change": "current_winning_bytes_preserved", "validation": "PASS", "errors": [],
                  "change_metrics": {"byte_exact_current_preservation": True, "raster_processing": False,
                                     "recompression": False, "source": "current winning aliases, not stock PNG"},
                  "visual_review": "No new art; stable current resources retained byte-for-byte",
                  "export_command": None}
        (output_dir/"metadata"/f"{row['id']}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        return record
    from PIL import Image
    import numpy as np
    ident = row["id"]
    meta_path = output_dir/"metadata"/f"{ident}.json"
    master, dds = output_dir/"png"/f"{ident}.png", output_dir/"dds"/f"{ident}.dds"
    source_path = Path(row["source_png"])
    fingerprint = record_fingerprint(row, receipts)
    source_digest = sha256(source_path)
    if meta_path.exists() and master.exists() and dds.exists():
        existing = json.loads(meta_path.read_text(encoding="utf-8"))
        if (existing.get("fingerprint") == fingerprint and existing.get("source_png_sha256") == source_digest
                and existing.get("validation") == "PASS" and existing.get("dds_sha256") == sha256(dds)
                and existing.get("png_sha256") == sha256(master)):
            dds_audit(dds.read_bytes())
            return existing
    with Image.open(source_path) as source:
        source.load()
        if source.size != (row["source_width"], row["source_height"]):
            raise ValueError(f"Source/manifest mismatch for {ident}")
        result, artistic = render(row, source, materials)
        metrics = change_metrics(source, result)
        expected_alpha = resize_source(source, result.size).getchannel("A") if "A" in source.getbands() else None
        alpha_exact = expected_alpha is None and "A" not in result.getbands()
        if expected_alpha is not None:
            alpha_exact = np.array_equal(np.asarray(expected_alpha), np.asarray(result.getchannel("A")))
        result.save(master)
    errors = []
    if not alpha_exact:
        errors.append("master_alpha_changed")
    if result.size != (row["target_width"], row["target_height"]):
        errors.append("target_dimensions")
    if result.width*row["source_height"] != result.height*row["source_width"]:
        errors.append("aspect_ratio_changed")
    if artistic["source_edge_delta"] is not None and artistic["source_edge_delta"] > 1/255:
        errors.append("protected_source_edges_changed")
    if row["route"] == "world_material_reinterpretation":
        pixels = np.asarray(result.convert("RGB"), dtype=np.float32)/255
        wrap_error = float((np.abs(pixels[:, 0]-pixels[:, -1]).mean()+np.abs(pixels[0]-pixels[-1]).mean())*.5)
        metrics["target_wrap_edge_error_full_resolution"] = wrap_error
        if wrap_error > 1/255:
            errors.append("world_wrap_edge_mismatch")
    artistic_change = "faithfully_preserved" if row["route"].startswith(FAITHFUL) else "material_reinterpreted"
    if artistic_change == "material_reinterpreted" and metrics["pixels_changed_over_5_255"] < .02:
        artistic_change = "no_effective_change"
        errors.append("insufficient_artistic_change")
    fmt = row["dds_format"].lower()
    if expected_alpha is None:
        fmt = "bc1"
    command = [str(exporter), "--format", fmt, "--quality", "production", "--mips", "--mip-filter", "kaiser",
               "--mip-gamma-correct", "--mip-pre-alpha", "--output", str(dds), str(master)]
    cutout = is_cutout(row, fmt)
    if expected_alpha is not None and cutout:
        # The exporter explicitly supports opacity preservation at higher mip
        # levels. Never apply a binary alpha policy to soft smoke/transparency.
        command += ["--scale-alpha", "--alpha-threshold", "127"]
    completed = subprocess.run(command, cwd=exporter.parent, capture_output=True, text=True, timeout=300)
    (output_dir/"logs"/f"{ident}.log").write_text(completed.stdout+"\n"+completed.stderr, encoding="utf-8")
    if completed.returncode or not dds.exists():
        raise RuntimeError(f"DDS exporter failed for {ident}: {completed.stderr[-500:]}")
    audit = dds_audit(dds.read_bytes())
    if (audit["width"], audit["height"]) != result.size:
        errors.append("dds_dimensions")
    dds_alpha_metrics = None
    alpha_mip_audit = None
    if expected_alpha is not None:
        with Image.open(dds) as decoded:
            decoded.load()
            dds_alpha_metrics = alpha_coverage_error(expected_alpha, decoded.convert("RGBA").getchannel("A"))
        errors.extend(alpha_validation_errors(dds_alpha_metrics, fmt == "bc1a"))
        alpha_mip_audit = audit_mip_alpha(dds.read_bytes(), expected_alpha, cutout=cutout)
        errors.extend(alpha_mip_audit["errors"])
    record = {**row, **artistic, "fingerprint": fingerprint, "source_png_sha256": source_digest,
              "master": str(master), "output": str(dds), "png_sha256": sha256(master), "dds_sha256": sha256(dds),
              "dds_format": fmt, "dds_audit": {key: value for key, value in audit.items() if key != "levels"},
              "master_alpha_exact": alpha_exact, "dds_alpha_metrics": dds_alpha_metrics, "alpha_mip_audit": alpha_mip_audit, "change_metrics": metrics,
              "artistic_change": artistic_change, "validation": "PASS" if not errors else "FAIL", "errors": errors,
              "visual_review": "pending", "export_command": command}
    meta_path.write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record


def contact_sheets(records, output_dir):
    from PIL import Image, ImageDraw
    groups = collections.defaultdict(list)
    for record in records:
        if record.get("validation") == "PASS" and record.get("output_strategy") != "current_winning_bytes":
            groups[record["route"]].append(record)
    for route, group in groups.items():
        selected = [group[round(i*(len(group)-1)/max(1, min(12, len(group))-1))] for i in range(min(12, len(group)))]
        sheet = Image.new("RGB", (1536, 80+len(selected)*245), (20, 22, 24))
        draw = ImageDraw.Draw(sheet)
        draw.text((12, 10), f"{route} | STOCK / CURRENT WINNING ART / NEW OUTPUT | alpha shown over checkerboard", fill="white")
        for index, row in enumerate(selected):
            top = 44+index*245
            current = row["winning_sources"]
            dds_sources = [item for item in current if item["entry"].lower().endswith(".dds")]
            chosen = (dds_sources or current)[-1] if current else None
            for col in range(3):
                if col == 0:
                    image = Image.open(row["source_png"])
                elif col == 2:
                    image = Image.open(row["master"])
                elif chosen:
                    import io
                    with zipfile.ZipFile(chosen["archive"]) as archive:
                        image = Image.open(io.BytesIO(archive.read(chosen["entry"])))
                else:
                    continue
                image.load()
                image.thumbnail((500, 210), Image.Resampling.LANCZOS)
                panel = Image.new("RGBA", image.size)
                checker = ImageDraw.Draw(panel)
                for yy in range(0, image.height, 16):
                    for xx in range(0, image.width, 16):
                        gray = 72 if (xx//16+yy//16)%2 else 120
                        checker.rectangle((xx, yy, xx+15, yy+15), fill=(gray, gray, gray, 255))
                panel.alpha_composite(image.convert("RGBA"))
                sheet.paste(panel.convert("RGB"), (col*512+6, top))
            draw.text((10, top+215), f"{row['id']} {row['asset_path']} | {row['artistic_change']}", fill=(225, 200, 145))
        sheet.save(output_dir/"review"/f"{route}.jpg", quality=94)


def package_alias_audit(records: list[dict], entries: list[dict]) -> dict:
    names = [entry["output_asset_path"].replace("\\", "/").lower() for entry in entries]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate packaged physical aliases")
    actual = set(names)
    expected = {source["entry"].replace("\\", "/").lower() for row in records if row["route"].startswith(FAITHFUL)
                for source in row["winning_sources"]}
    missing = sorted(expected-actual)
    logical_expected = {logical_name(row["asset_path"]) for row in records}
    logical_actual = {logical_name(name) for name in names}
    logical_missing = sorted(logical_expected-logical_actual)
    logical_extra = sorted(logical_actual-logical_expected)
    if missing or logical_missing or logical_extra:
        raise ValueError(f"Package alias/logical coverage failure: preserved={missing}, logical={logical_missing}, extra={logical_extra}")
    reinterpretation = [row for row in records if not row["route"].startswith(FAITHFUL)]
    mapping = [{"source_alias": source["entry"], "new_dds": row["output_asset_path"]}
               for row in reinterpretation for source in row["winning_sources"]]
    return {"preserved_physical_aliases": len(expected), "missing_preserved_aliases": missing,
            "packaged_physical_paths": len(actual), "logical_assets": len(logical_actual),
            "missing_logical_assets": logical_missing, "extra_logical_assets": logical_extra,
            "reinterpretation_source_alias_to_dds": mapping,
            "reinterpretation_alias_policy": "New DDS per logical source; stock fallback aliases remain in stock archives; engine DDS-resolution review required"}


def package(records, output_dir, source_main):
    """Stage new image archives and lossless non-image retention separately."""
    if any(row.get("validation") != "PASS" for row in records):
        raise ValueError("No release package is created while any asset fails")
    dest = output_dir/"packages"
    dest.mkdir(exist_ok=True)
    entries = []
    for row in records:
        if row.get("output_strategy") == "current_winning_bytes":
            entries.extend({**entry, "id": row["id"], "input_strategy": "current_winning_bytes"}
                           for entry in row["output_entries"])
        else:
            entries.append({"id": row["id"], "output_asset_path": row["output_asset_path"], "output": row["output"],
                            "sha256": row["dds_sha256"], "bytes": Path(row["output"]).stat().st_size,
                            "input_strategy": "generated_dds"})
    alias_report = package_alias_audit(records, entries)
    batches, batch, total = [], [], 0
    for entry in sorted(entries, key=lambda rec: rec["output_asset_path"].lower()):
        size = entry["bytes"]
        if batch and total+size > 900*1024**2:
            batches.append(batch)
            batch, total = [], 0
        batch.append(entry)
        total += size
    if batch:
        batches.append(batch)
    archives = []
    for index, batch in enumerate(batches, 1):
        path = dest/f"zzzzvreinterpret{index:02}.pk3"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=4, allowZip64=False) as archive:
            for entry in batch:
                if entry["input_strategy"] == "current_winning_bytes":
                    archive.writestr(entry["output_asset_path"], current_entry_bytes(entry))
                else:
                    archive.write(entry["output"], entry["output_asset_path"])
        with zipfile.ZipFile(path) as archive:
            if archive.testzip():
                raise ValueError(f"Archive CRC validation failed: {path}")
            if len(archive.namelist()) != len(batch):
                raise ValueError("Archive count mismatch")
            for entry in batch:
                if hashlib.sha256(archive.read(entry["output_asset_path"])).hexdigest() != entry["sha256"]:
                    raise ValueError(f"Packaged content hash mismatch: {entry['id']} {entry['output_asset_path']}")
        archives.append({"path": str(path), "images": len(batch), "sha256": sha256(path)})
    # Only optional third-party/reconstruction packs, never stock originals.
    # Root owns installation/disable. These staged retention files keep every
    # non-image entry byte-identical if an old image-bearing pack is disabled.
    retention = []
    for pack in sorted(source_main.glob("*.pk3"), key=lambda p: p.name.lower()):
        if pack.name.lower().startswith(("pak", "localized_english_pak")):
            continue
        with zipfile.ZipFile(pack) as source:
            entries = [entry for entry in source.infolist() if not entry.is_dir()
                       and Path(entry.filename).suffix.lower() not in IMAGE_EXTENSIONS]
            if not entries:
                continue
            keep = dest/f"{pack.stem}_nonimages.pk3"
            with zipfile.ZipFile(keep, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=4) as archive:
                for entry in entries:
                    archive.writestr(entry.filename, source.read(entry.filename))
            with zipfile.ZipFile(keep) as archive:
                if archive.testzip():
                    raise ValueError(f"Retention CRC failure: {keep}")
                for entry in entries:
                    if archive.read(entry.filename) != source.read(entry.filename):
                        raise ValueError(f"Non-image content changed: {entry.filename}")
            retention.append({"source": str(pack), "retained_archive": str(keep), "entries": len(entries),
                              "sha256": sha256(keep), "entry_bytes_preserved": True})
    return {"images": archives, "optional_nonimage_retention": retention, "alias_coverage": alias_report,
            "deployment": "not_installed; root must check engine mount/order and selectively disable old image packs",
            "aliases": "All current physical aliases retained for preservation; one new DDS per reinterpreted logical name; no duplicate output paths"}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=PRIOR/"production_manifest.csv")
    parser.add_argument("--main", type=Path, default=GAME_MAIN)
    parser.add_argument("--materials", type=Path, default=PROJECT/"assets"/"materials")
    parser.add_argument("--material-provenance", type=Path)
    parser.add_argument("--overrides", type=Path, default=PROJECT/"assets"/"overrides")
    parser.add_argument("--override-provenance", type=Path)
    parser.add_argument("--protection-masks", type=Path, default=PROJECT/"assets"/"protection_masks")
    parser.add_argument("--output", type=Path, default=PROJECT/"texture-production")
    parser.add_argument("--exporter", type=Path, default=EXPORTER)
    parser.add_argument("--max-dimension", type=int, choices=(1024, 2048, 4096), default=2048)
    parser.add_argument("--plan-out", type=Path, help="Optional JSON plan write; no image processing")
    parser.add_argument("--build", action="store_true", help="Explicitly process images/export DDS; does not deploy")
    parser.add_argument("--package", action="store_true", help="With --build, stage PK3s and lossless non-image retention")
    parser.add_argument("--ids", help="Comma-separated pilot IDs; coverage remains partial")
    parser.add_argument("--limit", type=int, help="Pilot size; never reported as full coverage")
    args = parser.parse_args(argv)
    if args.package and not args.build:
        parser.error("--package requires --build")
    rows = load_rows(args.manifest)
    winners = winning_paths(args.main)
    source_coverage = source_coverage_audit(rows, winners)
    if not source_coverage["complete_logical_coverage"]:
        raise ValueError(f"Active texture/source-manifest coverage mismatch: {source_coverage}")
    plans = [plan_row(row, winners, args.max_dimension) for row in rows]
    attach_art_inputs(plans, args.overrides, args.protection_masks,
                      args.override_provenance or args.overrides/"provenance.json")
    if args.ids:
        selected = set(args.ids.split(","))
        plans = [row for row in plans if row["id"] in selected]
        if {row["id"] for row in plans} != selected:
            raise ValueError("One or more requested pilot IDs do not exist")
    if args.limit:
        plans = plans[:args.limit]
    if args.plan_out:
        args.plan_out.parent.mkdir(parents=True, exist_ok=True)
        args.plan_out.write_text(json.dumps(plans, indent=2), encoding="utf-8")
    if not args.build:
        report = coverage_report(plans, complete=False)
        report["source_path_coverage"] = source_coverage
        print(json.dumps(report, indent=2))
        return 0
    if not args.exporter.exists():
        raise FileNotFoundError(f"DDS exporter missing: {args.exporter}")
    materials, receipts = load_materials(args.materials, args.material_provenance or args.materials/"provenance.json")
    for sub in ("png", "dds", "metadata", "logs", "review"):
        (args.output/sub).mkdir(parents=True, exist_ok=True)
    (args.output/"material_provenance.json").write_text(json.dumps(receipts, indent=2), encoding="utf-8")
    records, failures = [], []
    began = time.monotonic()
    for index, row in enumerate(plans, 1):
        try:
            record = process(row, args.output, materials, receipts, args.exporter)
            if record["validation"] == "FAIL":
                try:
                    fallback = candidate_preservation_plan(row, record["errors"])
                except ValueError:
                    pass
                else:
                    rejected = {key: record.get(key) for key in ("errors", "change_metrics", "alpha_mip_audit", "dds_sha256", "png_sha256")}
                    record = process(fallback, args.output, materials, receipts, args.exporter)
                    record["rejected_candidate"] = rejected
                    (args.output/"metadata"/f"{row['id']}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        except Exception as error:
            record = {**row, "validation": "FAIL", "errors": [str(error)], "visual_review": "pending"}
        records.append(record)
        if record["validation"] == "FAIL":
            failures.append(record)
        if index % 25 == 0 or index == len(plans):
            print(f"textures {index}/{len(plans)} static failures={len(failures)} elapsed={time.monotonic()-began:.0f}s", flush=True)
            (args.output/"manifest.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    complete = len(plans) == len(rows) and not failures
    report = coverage_report(records, complete=complete)
    report["source_manifest_assets"] = len(rows)
    report["source_path_coverage"] = source_coverage
    report["elapsed_seconds"] = round(time.monotonic()-began, 1)
    (args.output/"coverage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (args.output/"failures.json").write_text(json.dumps(failures, indent=2), encoding="utf-8")
    contact_sheets(records, args.output)
    if args.package:
        if not complete:
            raise ValueError("A release package requires the full source manifest and zero static failures")
        package_report = package(records, args.output, args.main)
        (args.output/"package.json").write_text(json.dumps(package_report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
