"""Coordinator checks; no raster processing, DDS exporter or installed writes."""
import contextlib
import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / "tools" / "build_texture_release.py"
RELEASE = None
if PATH.exists():
    spec = importlib.util.spec_from_file_location("build_texture_release", PATH)
    RELEASE = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(RELEASE)

spec = importlib.util.spec_from_file_location("release_fixture_pipeline", ROOT / "tools" / "reinterpret_textures.py")
PIPE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(PIPE)


def row(ident):
    return {"id": ident, "asset_path": f"gfx/ui/{ident}.tga", "category": "HUD / menu / UI / fonts",
            "material": "ui", "source_width": 2, "source_height": 2, "target_width": 2,
            "target_height": 2, "source_alpha": False, "likely_tile": False, "contains_text": False,
            "source_png": f"masters/{ident}.png", "dds_format": "bc1"}


class TextureReleaseTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(RELEASE, "The bounded release coordinator has not been implemented")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.main = self.base / "installed" / "Main"
        self.main.mkdir(parents=True)
        self.output = self.base / "staged"
        self.rows = [row("0003"), row("0001"), row("0002")]
        header = bytearray(18)
        struct.pack_into("<2H", header, 12, 2, 2)
        self.alias_bytes = bytes(header) + b"unchanged current texture bytes"
        with zipfile.ZipFile(self.main / "pak0.pk3", "w") as archive:
            for source in self.rows:
                archive.writestr(source["asset_path"], self.alias_bytes)
        self.winners = PIPE.winning_paths(self.main)
        self.plans = [PIPE.plan_row(source, self.winners, 2048) for source in self.rows]
        self.source_coverage = PIPE.source_coverage_audit(self.rows, self.winners)
        self.pipe = SimpleNamespace(**{name: getattr(PIPE, name) for name in (
            "load_rows", "winning_paths", "source_coverage_audit", "plan_row", "attach_art_inputs",
            "candidate_preservation_plan", "coverage_report", "package", "sha256", "process")})
        self.pipe.contact_sheets = lambda records, output: None
        self.pipe.load_materials = lambda folder, provenance: ({}, {})
        for name in ("PRIOR", "GAME_MAIN", "PROJECT", "EXPORTER"):
            setattr(self.pipe, name, getattr(PIPE, name))
        self.exporter = self.base / "unused_exporter.exe"
        self.exporter.touch()

    def build(self, plans=None, **changes):
        arguments = dict(plans=self.plans if plans is None else plans, source_rows=self.rows,
                         source_coverage=self.source_coverage, output=self.output, source_main=self.main,
                         materials={}, receipts={}, exporter=self.exporter, workers=2,
                         package_requested=False, pilot=False, pipe=self.pipe)
        arguments.update(changes)
        with contextlib.redirect_stdout(io.StringIO()):
            return RELEASE.build_release(**arguments)

    def test_full_pass_packages_every_current_alias_byte_exact(self):
        report = self.build(package_requested=True)
        self.assertTrue(report["complete"])
        self.assertTrue(report["release_ready"])
        package = json.loads((self.output / "package.json").read_text())
        self.assertEqual(len(package["images"]), 1)
        path = Path(package["images"][0]["path"])
        self.assertTrue(path.is_absolute())
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(set(archive.namelist()), {"gfx/ui/0001.tga", "gfx/ui/0002.tga", "gfx/ui/0003.tga"})
            self.assertEqual(archive.read("gfx/ui/0002.tga"), self.alias_bytes)
        self.assertEqual(PIPE.sha256(path), package["images"][0]["sha256"])

    def test_partial_or_pilot_input_never_packages(self):
        for plans, pilot in ((self.plans[:1], False), (self.plans, True)):
            with self.subTest(count=len(plans), pilot=pilot):
                with self.assertRaisesRegex(ValueError, "full|pilot"):
                    self.build(plans, pilot=pilot, package_requested=True)
                self.assertFalse((self.output / "packages").exists())
        report = self.build(self.plans[:1], pilot=True)
        self.assertFalse(report["complete"])
        self.assertFalse(report["release_ready"])

    def test_duplicate_ids_and_missing_requested_ids_reject_before_output(self):
        for plans in (self.plans + [self.plans[0]], [dict(self.plans[0], id="0009")]):
            with self.subTest(ids=[item["id"] for item in plans]):
                with self.assertRaises(ValueError):
                    self.build(plans)
                self.assertFalse(self.output.exists())
        for ids in ("0001,0001", "0001,9999", "0001,", ""):
            with self.subTest(ids=ids):
                with self.assertRaises(ValueError):
                    RELEASE.select_plans(self.plans, ids)
        with self.assertRaises(ValueError):
            RELEASE.select_plans(self.plans + [self.plans[0]], None)

    def test_path_like_and_case_colliding_ids_never_share_worker_files(self):
        for plans in ([dict(self.plans[0], id="../escape")],
                      [dict(self.plans[0], id="A"), dict(self.plans[1], id="a")]):
            with self.subTest(ids=[item["id"] for item in plans]):
                with self.assertRaises(ValueError):
                    RELEASE.select_plans(plans, None)

    def test_reverse_completion_is_written_in_stable_source_id_order(self):
        original = self.pipe.process
        finished = []

        def delayed(source, *args):
            if source["id"] == "0001":
                time.sleep(.06)
            record = original(source, *args)
            finished.append(source["id"])
            return record

        self.pipe.process = delayed
        self.build()
        self.assertNotEqual(finished, ["0001", "0002", "0003"])
        records = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual([entry["id"] for entry in records], ["0001", "0002", "0003"])
        self.assertEqual(json.loads((self.output / "failures.json").read_text()), [])

    def test_workers_are_bounded_and_invalid_inputs_do_not_start_work(self):
        for value in (0, -1, 3, 8, True, 1.5):
            with self.subTest(workers=value):
                with self.assertRaises(ValueError):
                    self.build(workers=value)
                self.assertFalse(self.output.exists())
        original = self.pipe.process
        lock = threading.Lock()
        active, peak = 0, 0

        def delayed(source, *args):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
            time.sleep(.02)
            try:
                return original(source, *args)
            finally:
                with lock:
                    active -= 1

        self.pipe.process = delayed
        report = self.build()
        self.assertTrue(report["complete"])
        self.assertEqual(peak, 2)

    def test_only_approved_candidate_failures_fall_back_to_current_aliases(self):
        original = self.pipe.process

        def candidate(source, *args):
            if not source["route"].startswith("faithful_"):
                return {**source, "validation": "FAIL", "errors": ["mip_7:cutout_mip_coverage"],
                        "change_metrics": {}, "alpha_mip_audit": {"errors": ["mip_7:cutout_mip_coverage"]}}
            return original(source, *args)

        self.pipe.process = candidate
        changed = [dict(source, route="bespoke_source_locked_reinterpretation") for source in self.plans]
        report = self.build(changed, package_requested=True)
        self.assertTrue(report["complete"])
        record = json.loads((self.output / "metadata" / "0001.json").read_text())
        self.assertEqual(record["route"], "faithful_rejected_candidate_preservation")
        self.assertEqual(record["rejected_candidate"]["errors"], ["mip_7:cutout_mip_coverage"])
        self.assertEqual(record["artistic_change"], "current_winning_bytes_preserved")
        package = json.loads((self.output / "package.json").read_text())
        with zipfile.ZipFile(package["images"][0]["path"]) as archive:
            self.assertEqual(archive.read("gfx/ui/0001.tga"), self.alias_bytes)

    def test_unapproved_failure_or_exception_keeps_release_blocked(self):
        for reason in ("protected_source_edges_changed", "exception"):
            with self.subTest(reason=reason):
                def failed(source, *args):
                    if reason == "exception":
                        raise RuntimeError("exporter unavailable")
                    return {**source, "validation": "FAIL", "errors": [reason]}
                self.pipe.process = failed
                with self.assertRaisesRegex(ValueError, "zero|failed"):
                    self.build(package_requested=True)
                self.assertFalse((self.output / "packages").exists())
                records = json.loads((self.output / "failures.json").read_text())
                self.assertEqual(len(records), 3)
                self.assertEqual([record["id"] for record in records], ["0001", "0002", "0003"])
                self.assertNotIn("rejected_candidate", records[0])
                self.assertFalse(json.loads((self.output / "coverage.json").read_text())["release_ready"])

    def test_source_coverage_mismatch_rejects_without_processing(self):
        with self.assertRaisesRegex(ValueError, "coverage"):
            self.build(source_coverage={**self.source_coverage, "complete_logical_coverage": False})
        self.assertFalse(self.output.exists())

    def test_review_or_packaging_exception_cannot_leave_release_ready(self):
        for phase in ("contact_sheets", "package"):
            with self.subTest(phase=phase):
                self.pipe.contact_sheets = lambda records, output: None
                self.pipe.package = PIPE.package

                def failed(*args):
                    raise RuntimeError(f"{phase} unavailable")

                setattr(self.pipe, phase, failed)
                with self.assertRaisesRegex(RuntimeError, "unavailable"):
                    self.build(package_requested=True)
                report = json.loads((self.output / "coverage.json").read_text())
                self.assertFalse(report["release_ready"])
                self.assertIn("unavailable", report["finalization_errors"][0])
                self.assertFalse((self.output / "package.json").exists())

    def test_cli_resolves_relative_outputs_and_inventory_does_not_build(self):
        manifest = self.base / "source.csv"
        with manifest.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=self.rows[0].keys())
            writer.writeheader()
            writer.writerows(self.rows)
        previous = Path.cwd()
        os.chdir(self.base)
        self.addCleanup(os.chdir, previous)
        arguments = ["--manifest", "source.csv", "--main", "installed/Main", "--output", "staged",
                     "--materials", "materials", "--exporter", "unused_exporter.exe"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(RELEASE.main(arguments, pipe=self.pipe), 0)
        self.assertFalse(self.output.exists())
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(RELEASE.main(arguments + ["--build"], pipe=self.pipe), 0)
        records = json.loads((self.output / "manifest.json").read_text())
        self.assertTrue(Path(records[0]["source_png"]).is_absolute())
        self.assertTrue(Path(records[0]["winning_sources"][0]["archive"]).is_absolute())
        self.assertEqual(json.loads((self.output / "coverage.json").read_text())["output"], str(self.output.resolve()))

    def test_cli_cannot_write_into_installed_or_source_inputs(self):
        manifest = self.base / "source.csv"
        with manifest.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=self.rows[0].keys())
            writer.writeheader()
            writer.writerows(self.rows)
        for output in (self.main / "new", self.main.parent / "new", self.base / "masters" / "new"):
            with self.subTest(output=output):
                with self.assertRaises(ValueError), contextlib.redirect_stdout(io.StringIO()):
                    RELEASE.main(["--manifest", str(manifest), "--main", str(self.main), "--output", str(output),
                                  "--build", "--exporter", str(self.exporter)], pipe=self.pipe)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
