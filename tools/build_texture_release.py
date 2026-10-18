#!/usr/bin/env python3
"""Bounded, checkpointed staging coordinator for the existing texture pipeline.

No raster work is implied: --build is required; --package only stages archives
after full source coverage and zero failures. Pilot IDs never create a release.
The two workers share already-loaded material donors read-only, and each source
ID owns its existing independent PNG/DDS/metadata paths. Nothing is installed.
"""
from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
import importlib.util
import json
from pathlib import Path
import re
import time


_SPEC = importlib.util.spec_from_file_location("texture_release_pipeline", Path(__file__).with_name("reinterpret_textures.py"))
PIPE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(PIPE)


def select_plans(plans: list[dict], ids: str | None) -> list[dict]:
    """Validate per-ID file isolation, then select in deterministic ID order."""
    seen = set()
    for row in plans:
        ident = row.get("id")
        if (not isinstance(ident, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", ident)
                or re.fullmatch(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])", ident, re.I)):
            raise ValueError(f"Invalid source ID for independent worker paths: {ident!r}")
        if ident.casefold() in seen:
            raise ValueError(f"Duplicate source ID: {ident}")
        seen.add(ident.casefold())
    if ids is not None:
        requested = [ident.strip() for ident in ids.split(",")]
        if not all(requested) or len(set(requested)) != len(requested):
            raise ValueError("Pilot IDs must be nonempty and unique")
        if set(requested) - {row["id"] for row in plans}:
            raise ValueError("One or more requested pilot IDs do not exist")
        plans = [row for row in plans if row["id"] in set(requested)]
    return sorted(plans, key=lambda row: (0, int(row["id"]), row["id"]) if row["id"].isdigit()
                  else (1, row["id"].casefold(), row["id"]))


def validate_output(output: Path, source_main: Path, protected: list[Path]) -> Path:
    """Never allow the staging tree to overlap installed or input resources."""
    output = output.resolve()
    for root in [source_main.resolve().parent, *protected]:
        root = root.resolve()
        if output == root or output.is_relative_to(root) or root.is_relative_to(output):
            raise ValueError(f"Staging output overlaps installed/source inputs: {output} and {root}")
    return output


def write_json(path: Path, value) -> None:
    # Worker threads never write these shared root records. Replacement leaves
    # the previous complete checkpoint available if serialization is interrupted.
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2), encoding="utf-8")
    temporary.replace(path)


def build_release(*, plans: list[dict], source_rows: list[dict], source_coverage: dict,
                  output: Path, source_main: Path, materials: dict, receipts: dict,
                  exporter: Path, workers: int = 2, package_requested: bool = False,
                  pilot: bool = False, pipe=PIPE) -> dict:
    """Run no more than two pending jobs and checkpoint every 25 completions."""
    if type(workers) is not int or workers not in (1, 2):
        raise ValueError("workers must be 1 or 2")
    ordered = select_plans(plans, None)
    sources = select_plans(source_rows, None)
    source_ids = {row["id"] for row in sources}
    selected_ids = {row["id"] for row in ordered}
    if not ordered or selected_ids - source_ids:
        raise ValueError("Build IDs are empty or missing from the source manifest")
    if not source_coverage.get("complete_logical_coverage"):
        raise ValueError("Active texture/source-manifest coverage is incomplete")
    full_input = selected_ids == source_ids and not pilot
    if package_requested and not full_input:
        raise ValueError("A package requires the full source manifest, not a pilot")
    source_main = source_main.resolve()
    exporter = exporter.resolve()
    output = validate_output(output, source_main,
                             [Path(row["source_png"]).resolve().parent for row in sources])
    for sub in ("png", "dds", "metadata", "logs", "review"):
        (output / sub).mkdir(parents=True, exist_ok=True)
    write_json(output / "material_provenance.json", receipts)
    finished = {}
    began = time.monotonic()

    def checkpoint() -> dict:
        records = [finished[row["id"]] for row in ordered if row["id"] in finished]
        failures = [row for row in records if row.get("validation") != "PASS"]
        complete = full_input and len(records) == len(sources) and not failures
        report = pipe.coverage_report(records, complete=complete)
        report.update(source_manifest_assets=len(sources), selected_assets=len(ordered),
                      completed_assets=len(records), source_path_coverage=source_coverage,
                      elapsed_seconds=round(time.monotonic() - began, 1), output=str(output),
                      workers=workers, pilot=pilot, release_ready=complete,
                      release_status="static_passed_not_packaged" if complete else "blocked_or_in_progress",
                      visual_validation="Static checks only; root still owns visual/UV review and deployment")
        write_json(output / "manifest.json", records)
        write_json(output / "coverage.json", report)
        write_json(output / "failures.json", failures)
        print(f"textures {len(records)}/{len(ordered)} static failures={len(failures)} "
              f"elapsed={time.monotonic() - began:.0f}s output={output}", flush=True)
        return report

    def process_one(row: dict) -> dict:
        try:
            record = pipe.process(row, output, materials, receipts, exporter)
            if record["validation"] == "FAIL":
                try:
                    fallback = pipe.candidate_preservation_plan(row, record["errors"])
                except ValueError:
                    pass
                else:
                    rejected = {key: record.get(key) for key in (
                        "errors", "change_metrics", "alpha_mip_audit", "dds_sha256", "png_sha256")}
                    record = pipe.process(fallback, output, materials, receipts, exporter)
                    record["rejected_candidate"] = rejected
                    write_json(output / "metadata" / f"{row['id']}.json", record)
            if record.get("id") != row["id"] or record.get("validation") not in {"PASS", "FAIL"}:
                raise ValueError("Processor returned a mismatched ID or invalid validation status")
            return record
        except Exception as error:
            record = {**row, "validation": "FAIL", "errors": [str(error)], "visual_review": "pending"}
            write_json(output / "metadata" / f"{row['id']}.json", record)
            return record

    checkpoint()  # A reused output is immediately marked in-progress, never stale-ready.
    remaining = iter(ordered)
    with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="texture") as executor:
        pending = {}
        for _ in range(min(workers, len(ordered))):
            row = next(remaining)
            pending[executor.submit(process_one, row)] = row
        while pending:
            done, _ = wait(pending, return_when=FIRST_COMPLETED)
            for future in done:
                row = pending.pop(future)
                finished[row["id"]] = future.result()
                if len(finished) % 25 == 0:
                    checkpoint()
                next_row = next(remaining, None)
                if next_row is not None:
                    pending[executor.submit(process_one, next_row)] = next_row
    report = checkpoint()
    records = [finished[row["id"]] for row in ordered]
    try:
        pipe.contact_sheets(records, output)
        if package_requested:
            if not report["release_ready"]:
                raise ValueError("A release package requires full source coverage and zero failed assets")
            package_report = pipe.package(records, output, source_main)
            write_json(output / "package.json", package_report)
            report["release_status"] = "packaged_not_installed"
            write_json(output / "coverage.json", report)
    except Exception as error:
        # Review/archive errors must not leave the successful static checkpoint
        # looking ready. Existing staged archives are never assumed admissible.
        report.update(release_ready=False, release_status="blocked_finalization",
                      finalization_errors=[str(error)])
        write_json(output / "coverage.json", report)
        raise
    return report


def main(argv=None, *, pipe=PIPE) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=pipe.PRIOR / "production_manifest.csv")
    parser.add_argument("--main", type=Path, default=pipe.GAME_MAIN)
    parser.add_argument("--materials", type=Path, default=pipe.PROJECT / "assets" / "materials")
    parser.add_argument("--material-provenance", type=Path)
    parser.add_argument("--overrides", type=Path, default=pipe.PROJECT / "assets" / "overrides")
    parser.add_argument("--override-provenance", type=Path)
    parser.add_argument("--protection-masks", type=Path, default=pipe.PROJECT / "assets" / "protection_masks")
    parser.add_argument("--output", type=Path, required=True, help="Distinct staging output; never the installed game")
    parser.add_argument("--exporter", type=Path, default=pipe.EXPORTER)
    parser.add_argument("--max-dimension", type=int, choices=(1024, 2048, 4096), default=2048)
    parser.add_argument("--workers", type=int, choices=(1, 2), default=2)
    parser.add_argument("--ids", help="Comma-separated pilot IDs; pilots can never package")
    parser.add_argument("--build", action="store_true", help="Explicitly process images/export DDS")
    parser.add_argument("--package", action="store_true", help="With --build, stage a fully validated release")
    args = parser.parse_args(argv)
    if args.package and not args.build:
        parser.error("--package requires --build")
    if args.package and args.ids is not None:
        parser.error("--package cannot be used with pilot --ids")
    for name in ("manifest", "main", "materials", "overrides", "protection_masks", "output", "exporter"):
        setattr(args, name, getattr(args, name).resolve())
    provenance = (args.material_provenance or args.materials / "provenance.json").resolve()
    overrides_provenance = (args.override_provenance or args.overrides / "provenance.json").resolve()
    rows = pipe.load_rows(args.manifest)
    for row in rows:
        source_png = Path(row["source_png"])
        row["source_png"] = str((source_png if source_png.is_absolute() else args.manifest.parent / source_png).resolve())
    rows = select_plans(rows, None)
    winners = pipe.winning_paths(args.main)
    source_coverage = pipe.source_coverage_audit(rows, winners)
    if not source_coverage["complete_logical_coverage"]:
        raise ValueError(f"Active texture/source-manifest coverage mismatch: {source_coverage}")
    args.output = validate_output(args.output, args.main, [args.materials, args.overrides, args.protection_masks,
                                 *[Path(row["source_png"]).parent for row in rows]])
    plans = [pipe.plan_row(row, winners, args.max_dimension) for row in rows]
    pipe.attach_art_inputs(plans, args.overrides, args.protection_masks, overrides_provenance)
    plans = select_plans(plans, args.ids)
    if not args.build:
        report = pipe.coverage_report(plans, complete=False)
        report.update(source_path_coverage=source_coverage, output=str(args.output), workers=args.workers,
                      pilot=args.ids is not None, release_ready=False)
        print(json.dumps(report, indent=2))
        return 0
    if not args.exporter.exists():
        raise FileNotFoundError(f"DDS exporter missing: {args.exporter}")
    materials, receipts = pipe.load_materials(args.materials, provenance)
    report = build_release(plans=plans, source_rows=rows, source_coverage=source_coverage, output=args.output,
                           source_main=args.main, materials=materials, receipts=receipts, exporter=args.exporter,
                           workers=args.workers, package_requested=args.package, pilot=args.ids is not None, pipe=pipe)
    print(json.dumps(report, indent=2))
    return 1 if report["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
