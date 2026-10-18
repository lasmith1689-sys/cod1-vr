"""Metadata/format tests; no raster processing, generator, exporter or game launch."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

PATH = Path(__file__).resolve().parents[1]/"tools"/"reinterpret_textures.py"
SPEC = importlib.util.spec_from_file_location("reinterpret_textures", PATH)
PIPE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PIPE)


def row(**changes):
    value = {"id": "1", "asset_path": "textures/normandy/walls/brick@wall.tga",
             "category": "architecture / walls / floors / terrain", "material": "brick",
             "source_width": 256, "source_height": 128, "target_width": 1024,
             "target_height": 512, "source_alpha": False, "likely_tile": True,
             "contains_text": False, "source_png": "unused.png"}
    value.update(changes)
    return value


def legacy_dds(width, height, fourcc=b"DXT1", count=None):
    count = max(width, height).bit_length() if count is None else count
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<2I", header, 12, height, width)
    struct.pack_into("<I", header, 28, count)
    header[84:88] = fourcc
    block = 8 if fourcc == b"DXT1" else 16
    size = sum(max(1, (max(1, width >> level)+3)//4)*max(1, (max(1, height >> level)+3)//4)*block for level in range(count))
    return bytes(header)+bytes(size)


class RoutingTests(unittest.TestCase):
    def test_world_material_is_reinterpreted(self):
        self.assertEqual(PIPE.route_for(row())[0], "world_material_reinterpretation")

    def test_full_reinterpretation_does_not_falsely_claim_functional_art(self):
        for category in PIPE.PRESERVE_CATEGORIES:
            with self.subTest(category=category):
                self.assertTrue(PIPE.route_for(row(category=category))[0].startswith("faithful_"))

    def test_mixed_prop_is_not_arbitrarily_metal(self):
        self.assertEqual(PIPE.route_for(row(category="props", material="generic_prop"))[0], "faithful_unclassified_preservation")

    def test_weapon_is_mixed_atlas_route(self):
        self.assertEqual(PIPE.route_for(row(category="weapon textures", material="weapon", likely_tile=False,
                                          asset_path="skins/viewmodel@m1carbine.dds"))[0], "weapon_atlas_material_reinterpretation")

    def test_alpha_cutout_has_alpha_route(self):
        self.assertEqual(PIPE.route_for(row(category="foliage / vegetation / alpha-cutout assets", source_alpha=True))[0],
                         "alpha_material_reinterpretation")

    def test_face_and_effect_semantics_are_preserved(self):
        for asset in ("skins/body@head.dds", "gfx/fx/smoke.dds", "skins/viewhands@default.jpg"):
            with self.subTest(asset=asset):
                self.assertTrue(PIPE.route_for(row(asset_path=asset))[0].startswith("faithful_"))

    def test_bare_hand_names_are_identity_not_generic_uniform_material(self):
        for asset in ("skins/body@characterhand.dds", "skins/body@wehrmacht_hands.dds", "skins/hand.tga", "skins/hands.tga"):
            with self.subTest(asset=asset):
                sample = row(asset_path=asset, category="character / uniform / skin textures", material="cloth")
                self.assertEqual(PIPE.route_for(sample)[0], "faithful_identity_preservation")

    def test_named_character_boots_are_leather_even_if_manifest_says_cloth(self):
        for asset in ("skins/body@air_boots.dds", "skins/body@british_jerkin.dds"):
            with self.subTest(asset=asset):
                sample = row(asset_path=asset, category="character / uniform / skin textures", material="cloth")
                self.assertEqual(PIPE.route_for(sample)[0], "faithful_unavailable_material_preservation")

    def test_sign_mask_and_text_override_general_material(self):
        self.assertEqual(PIPE.route_for(row(contains_text=True))[0], "faithful_marking_preservation")

    def test_cobble_and_bark_get_specific_materials(self):
        self.assertEqual(PIPE.material_for(row(asset_path="textures/road/cobbleroad.tga", material="stone")), "cobble")
        self.assertEqual(PIPE.material_for(row(asset_path="skins/wood@treebark.dds", material="wood")), "bark")

    def test_alpha_does_not_admit_unknown_material(self):
        sample = row(category="props", material="generic_prop", source_alpha=True, asset_path="skins/unknown.dds")
        self.assertEqual(PIPE.route_for(sample)[0], "faithful_unclassified_preservation")

    def test_leather_is_not_repainted_as_cloth(self):
        sample = row(category="character / uniform / skin textures", material="leather", asset_path="skins/body@boots.dds")
        self.assertEqual(PIPE.route_for(sample)[0], "faithful_unavailable_material_preservation")

    def test_size_keeps_rectangular_aspect(self):
        for width, height in ((128, 1024), (256, 512), (384, 448), (1024, 32), (512, 512)):
            for cap in (1024, 2048, 4096):
                sample = row(source_width=width, source_height=height, target_width=width*4, target_height=height*4)
                w, h = PIPE.target_size(sample, "world_material_reinterpretation", cap)
                self.assertEqual(w*height, h*width)
                self.assertLessEqual(max(w, h), max(cap, width, height))

    def test_non_power_of_two_source_is_not_exported_as_engine_rejected_dds(self):
        sample = row(source_width=384, source_height=448, material="metal", category="props")
        self.assertTrue(PIPE.route_for(sample)[0].startswith("faithful_"))

    def test_preservation_is_original_dimensions(self):
        self.assertEqual(PIPE.target_size(row(), "faithful_ui_preservation"), (256, 128))

    def test_aliases_generate_one_dds_logical_path(self):
        names = [PIPE.logical_name(name) for name in ("skins/ViewModel@M1.TGA", "skins/viewmodel@m1.dds", "skins\\viewmodel@m1.jpg")]
        self.assertEqual(len(set(names)), 1)
        planned = PIPE.plan_row(row(), {}, 2048)
        self.assertTrue(planned["output_asset_path"].endswith(".dds"))

    def test_coverage_never_calls_preserved_or_failed_assets_reinterpreted(self):
        records = [dict(row(), route="world_material_reinterpretation", validation="PASS", artistic_change="material_reinterpreted"),
                   dict(row(id="2"), route="faithful_ui_preservation", validation="PASS", artistic_change="faithfully_preserved"),
                   dict(row(id="3"), route="weapon_atlas_material_reinterpretation", validation="FAIL", artistic_change="material_reinterpreted")]
        report = PIPE.coverage_report(records, complete=True)
        self.assertEqual(report["material_reinterpreted_and_static_passed"], 1)
        self.assertEqual(report["faithfully_preserved"], 1)
        self.assertFalse(report["all_textures_reinterpreted"])


class FormatTests(unittest.TestCase):
    def test_all_legacy_compression_formats_and_rectangular_mips(self):
        for fourcc in (b"DXT1", b"DXT3", b"DXT5"):
            for width, height in ((1, 1), (1024, 128), (384, 448)):
                with self.subTest(fourcc=fourcc, size=(width, height)):
                    result = PIPE.dds_audit(legacy_dds(width, height, fourcc))
                    self.assertTrue(result["ends_at_1x1"])
                    self.assertEqual(result["trailing_bytes"], 0)

    def test_missing_tail_mip_reproduced_gray_texture_cause_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            PIPE.dds_audit(legacy_dds(1024, 1024, count=10))

    def test_payload_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "payload"):
            PIPE.dds_audit(legacy_dds(128, 256)+b"unexpected")
        with self.assertRaisesRegex(ValueError, "payload"):
            PIPE.dds_audit(legacy_dds(128, 256)[:-1])

    def test_dx10_is_not_silently_admitted_to_old_renderer(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            PIPE.dds_audit(legacy_dds(4, 4, b"DX10"))

    def test_alpha_coverage_rejects_changed_silhouette(self):
        metrics = {"mean_absolute": .001, "coverage_error_128": .025, "coverage_error_16": .002}
        self.assertIn("dds_alpha_coverage", PIPE.alpha_validation_errors(metrics, True))

    def test_faint_transparency_error_is_detected(self):
        metrics = {"mean_absolute": .001, "coverage_error_128": .001, "coverage_error_16": .02}
        self.assertIn("dds_faint_alpha_coverage", PIPE.alpha_validation_errors(metrics, False))
        self.assertNotIn("dds_faint_alpha_coverage", PIPE.alpha_validation_errors(metrics, True))

    def test_lower_mip_cutout_loss_is_not_hidden_by_passing_top_level(self):
        level = {"width": 32, "height": 32, "coverage_error_128": .18, "mean_alpha_error": .01}
        self.assertIn("cutout_mip_coverage", PIPE.mip_alpha_validation_errors(level, True))

    def test_low_mip_coverage_has_only_one_texel_allowance(self):
        level = {"width": 2, "height": 2, "coverage_error_128": .25, "mean_alpha_error": .01}
        self.assertEqual(PIPE.mip_alpha_validation_errors(level, True), [])
        level["coverage_error_128"] = .26
        self.assertIn("cutout_mip_coverage", PIPE.mip_alpha_validation_errors(level, True))

    def test_semantic_cutouts_include_bc3_foliage_but_not_soft_smoke(self):
        self.assertTrue(PIPE.is_cutout(row(category="foliage / vegetation / alpha-cutout assets"), "bc3"))
        self.assertFalse(PIPE.is_cutout(row(category="special-purpose technical textures", asset_path="gfx/fx/smoke.dds"), "bc3"))

    def test_soft_mip_opacity_is_separate_from_binary_coverage(self):
        level = {"width": 16, "height": 16, "coverage_error_128": .5, "mean_alpha_error": .01}
        self.assertEqual(PIPE.mip_alpha_validation_errors(level, False), [])
        level["mean_alpha_error"] = .08
        self.assertIn("soft_mip_mean_alpha", PIPE.mip_alpha_validation_errors(level, False))

    def test_bespoke_override_requires_provenance_and_layout_contract(self):
        self.assertIn("override_must_declare_source_locked_layout", PIPE.override_metadata_errors({}, "a", "b"))
        entry = {"prompt": "Source-locked repaint", "origin": "built_in_imagegen", "preserve_layout": True,
                 "sha256": "a", "source_png_sha256": "b"}
        self.assertEqual(PIPE.override_metadata_errors(entry, "a", "b"), [])
        self.assertIn("override_source_digest_mismatch", PIPE.override_metadata_errors(entry, "a", "changed"))


class CurrentBytePreservationTests(unittest.TestCase):
    def test_failed_alpha_candidate_can_only_fall_back_to_all_current_aliases(self):
        source = {"archive": "original.pk3", "entry": "textures/tree.tga", "width": 256, "height": 512}
        sample = dict(row(), winning_sources=[source], route="alpha_material_reinterpretation",
                      output_strategy="new_dds_from_stock_master")
        plan = PIPE.candidate_preservation_plan(sample, ["mip_9:cutout_mip_coverage"])
        self.assertEqual(plan["output_strategy"], "current_winning_bytes")
        self.assertEqual(plan["output_entries"][0]["output_asset_path"], "textures/tree.tga")
        self.assertEqual((plan["target_width"], plan["target_height"]), (256, 512))
        self.assertTrue(plan["route"].startswith("faithful_"))
        with self.assertRaises(ValueError):
            PIPE.candidate_preservation_plan(sample, ["aspect_ratio_changed"])

    def test_legacy_preservation_record_cannot_enter_stock_raster_export(self):
        sample = dict(row(), route="faithful_ui_preservation")
        try:
            PIPE.process(sample, Path("unused"), {}, {}, Path("absent-exporter.exe"))
        except Exception as error:
            self.assertIsInstance(error, ValueError)
            self.assertIn("preservation", str(error).lower())
        else:
            self.fail("A preservation route without current-byte inputs was admitted")

    def test_preservation_plan_keeps_each_current_alias_and_its_dimensions(self):
        aliases = [{"archive": "current.pk3", "entry": "ui/font.dds", "width": 1024, "height": 512},
                   {"archive": "stock.pk3", "entry": "ui/font.TGA", "width": 256, "height": 128}]
        sample = row(asset_path="ui/font.tga", category="HUD / menu / UI / fonts")
        planned = PIPE.plan_row(sample, {"ui/font": aliases}, 2048)
        self.assertEqual(planned.get("output_strategy"), "current_winning_bytes")
        self.assertEqual((planned["target_width"], planned["target_height"]), (1024, 512))
        self.assertEqual([entry["output_asset_path"] for entry in planned.get("output_entries", [])],
                         ["ui/font.dds", "ui/font.TGA"])
        self.assertEqual([(entry["width"], entry["height"]) for entry in planned["output_entries"]],
                         [(1024, 512), (256, 128)])

    def test_no_current_winner_cannot_silently_fall_back_to_stock(self):
        with self.assertRaisesRegex(ValueError, "winning"):
            PIPE.plan_row(row(category="HUD / menu / UI / fonts"), {}, 2048)

    def test_raw_header_dimensions_do_not_decode_or_recompress(self):
        self.assertTrue(hasattr(PIPE, "image_header_dimensions"), "Missing metadata-only current-image header reader")
        self.assertEqual(PIPE.image_header_dimensions(legacy_dds(1024, 128), ".dds"), (1024, 128))
        tga = bytearray(18)
        struct.pack_into("<2H", tga, 12, 256, 64)
        self.assertEqual(PIPE.image_header_dimensions(bytes(tga), ".tga"), (256, 64))

    def test_preservation_process_and_package_keep_alias_bytes_exact(self):
        with tempfile.TemporaryDirectory(prefix="codvr-preservation-test-") as folder:
            root = Path(folder)
            main, output = root/"main", root/"output"
            main.mkdir()
            (output/"metadata").mkdir(parents=True)
            stock = legacy_dds(256, 128)
            current = legacy_dds(1024, 512)
            with zipfile.ZipFile(main/"pak0.pk3", "w") as archive:
                archive.writestr("ui/font.TGA", bytes(18))
                archive.writestr("ui/font.dds", stock)
            with zipfile.ZipFile(main/"zzcustom.pk3", "w") as archive:
                archive.writestr("ui/font.dds", current)
                archive.writestr("scripts/functional.shader", b"original shader bytes")
            # The TGA header encodes no dimensions in this synthetic fixture;
            # use an explicit valid minimal header, not an image-generation job.
            tga = bytearray(18)
            struct.pack_into("<2H", tga, 12, 256, 128)
            with zipfile.ZipFile(main/"pak0.pk3", "w") as archive:
                archive.writestr("ui/font.TGA", bytes(tga))
                archive.writestr("ui/font.dds", stock)
            winners = PIPE.winning_paths(main)
            planned = PIPE.plan_row(row(asset_path="ui/font.tga", category="HUD / menu / UI / fonts"), winners, 2048)
            self.assertEqual(planned.get("output_strategy"), "current_winning_bytes")
            # Missing stock PNG/exporter prove preservation does not run raster code.
            record = PIPE.process(planned, output, {}, {}, root/"absent-exporter.exe")
            self.assertEqual(record.get("artistic_change"), "current_winning_bytes_preserved")
            self.assertEqual(record["validation"], "PASS")
            self.assertFalse((output/"png").exists())
            self.assertFalse((output/"dds").exists())
            report = PIPE.package([record], output, main)
            image_entries = {}
            for pack in report["images"]:
                with zipfile.ZipFile(pack["path"]) as archive:
                    image_entries.update({name: archive.read(name) for name in archive.namelist()})
            self.assertEqual(image_entries, {"ui/font.dds": current, "ui/font.TGA": bytes(tga)})
            self.assertEqual(report["alias_coverage"]["missing_preserved_aliases"], [])
            self.assertEqual(report["alias_coverage"]["preserved_physical_aliases"], 2)
            retention = report["optional_nonimage_retention"]
            self.assertEqual(len(retention), 1)
            with zipfile.ZipFile(retention[0]["retained_archive"]) as archive:
                self.assertEqual(archive.read("scripts/functional.shader"), b"original shader bytes")

    def test_missing_preservation_alias_is_a_release_failure(self):
        self.assertTrue(hasattr(PIPE, "package_alias_audit"), "Missing physical alias coverage gate")
        records = [{"id": "1", "asset_path": "ui/font.tga", "route": "faithful_ui_preservation",
                    "winning_sources": [{"entry": "ui/font.dds"}, {"entry": "ui/font.TGA"}]}]
        with self.assertRaisesRegex(ValueError, "alias"):
            PIPE.package_alias_audit(records, [{"output_asset_path": "ui/font.dds"}])

    def test_new_active_logical_asset_cannot_be_omitted_from_full_manifest(self):
        self.assertTrue(hasattr(PIPE, "source_coverage_audit"), "Missing full active asset coverage gate")
        winners = {"textures/known": [{"entry": "textures/known.dds"}],
                   "textures/extra": [{"entry": "textures/extra.dds"}]}
        report = PIPE.source_coverage_audit([row(asset_path="textures/known.dds")], winners)
        self.assertEqual(report["active_logical_not_in_manifest"], ["textures/extra"])
        self.assertFalse(report["complete_logical_coverage"])


if __name__ == "__main__":
    unittest.main()
