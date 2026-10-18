"""Real raster checks using the existing production image environment."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
import numpy as np
from PIL import Image

SPEC = importlib.util.spec_from_file_location("reinterpret", Path(__file__).resolve().parents[1]/"tools"/"reinterpret_textures.py")
PIPE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PIPE)


class MaterialFidelityTests(unittest.TestCase):
    def test_explicit_skin_lock_keeps_source_pixels_and_alpha(self):
        pixels = np.full((64, 64, 4), (150, 142, 97, 255), dtype=np.uint8)
        pixels[:, :32, :3] = (190, 140, 110)
        pixels[:, :, 3] = np.arange(64, dtype=np.uint8)[:, None]*4
        source = Image.fromarray(pixels)
        lock = np.zeros((64, 64), dtype=np.uint8)
        lock[:, :32] = 255
        with tempfile.TemporaryDirectory() as folder:
            mask_path = Path(folder)/"skin.png"
            Image.fromarray(lock).save(mask_path)
            row = dict(id="test", asset_path="skins/body@rollsleeve.dds", category="character / uniform / skin textures",
                       assigned_material="cloth", route="character_atlas_material_reinterpretation", target_width=128, target_height=128,
                       protection_mask={"file": str(mask_path)})
            donor = Image.new("RGB", (512, 512), (80, 120, 70))
            output, _ = PIPE.render(row, source, {"cloth": donor, "metal": donor})
            got = np.asarray(output)
            base = np.asarray(PIPE.resize_source(source, output.size))
            self.assertTrue(np.array_equal(got[:, :64], base[:, :64]))
            self.assertTrue(np.array_equal(got[:, :, 3], base[:, :, 3]))

    def test_uniform_donor_cannot_change_source_palette_or_add_macro_twill_patches(self):
        from scipy.ndimage import gaussian_filter
        pixels = np.full((128, 128, 3), (165, 149, 103), dtype=np.uint8)
        donor = np.full((512, 512, 3), (60, 88, 58), dtype=np.uint8)
        donor[:, ::64] = (180, 200, 170)
        donor[:, 32:64] = (120, 146, 108)
        source = Image.fromarray(pixels)
        row = dict(id="test", asset_path="skins/body@uniform.dds", category="character / uniform / skin textures",
                   assigned_material="cloth", route="character_atlas_material_reinterpretation", target_width=256, target_height=256)
        output, _ = PIPE.render(row, source, {"cloth": Image.fromarray(donor), "metal": Image.fromarray(donor)})
        got = np.asarray(output, dtype=float)/255
        base = np.asarray(PIPE.resize_source(source, output.size), dtype=float)/255
        self.assertLess(float(np.max(np.abs(got.mean((0, 1))-base.mean((0, 1))))), .025)
        broad_delta = gaussian_filter(got-base, 16, axes=(0, 1))
        self.assertLess(float(np.sqrt((broad_delta*broad_delta).mean())), .01)

    def test_cloth_prop_donor_also_preserves_original_palette(self):
        source = Image.new("RGB", (128, 128), (165, 149, 103))
        donor = Image.new("RGB", (512, 512), (60, 88, 58))
        row = dict(id="test", asset_path="skins/cloth@eureka_bag.dds", category="props",
                   assigned_material="cloth", route="structural_material_reinterpretation", target_width=256, target_height=256)
        output, _ = PIPE.render(row, source, {"cloth": donor})
        got = np.asarray(output, dtype=float)/255
        base = np.asarray(PIPE.resize_source(source, output.size), dtype=float)/255
        self.assertLess(float(np.max(np.abs(got.mean((0, 1))-base.mean((0, 1))))), .025)

    def test_material_blend_feathers_outside_but_never_inside_protected_features(self):
        protected = np.zeros((32, 32), dtype=bool)
        protected[:, :8] = True
        guard = PIPE.material_blend_guard(protected, 2)
        self.assertEqual(float(guard[:, :8].max()), 0)
        self.assertGreater(float(guard[16, 8]), 0)
        self.assertLess(float(guard[16, 8]), float(guard[16, 16]))
        self.assertEqual(float(guard[16, 20]), 1)

    def test_world_material_keeps_source_palette_and_mortar_structure(self):
        pixels = np.full((128, 128, 3), (120, 65, 45), dtype=np.uint8)
        pixels[30:35] = (35, 30, 27)
        pixels[62:67] = (35, 30, 27)
        pixels[:, 62:67] = (35, 30, 27)
        source = Image.fromarray(pixels)
        donor = np.full((512, 512, 3), (65, 155, 85), dtype=np.uint8)
        donor[::4] = (95, 185, 115)
        row = dict(id="test", asset_path="textures/wall/brick.tga", category="architecture / walls / floors / terrain",
                   assigned_material="brick", route="world_material_reinterpretation", target_width=256, target_height=256)
        output, _ = PIPE.render(row, source, {"brick": Image.fromarray(donor)})
        got = np.asarray(output, dtype=float)/255
        base = np.asarray(PIPE.resize_source(source, output.size), dtype=float)/255
        self.assertLess(float(np.max(np.abs(got.mean((0, 1))-base.mean((0, 1))))), .035)
        # Interior joints must still be dark, in their original coordinates.
        self.assertLess(float(got[62:68, 40:100].mean()), .19)

    def test_tree_grass_detail_does_not_repaint_brown_trunk(self):
        base = np.full((64, 64, 3), (.19, .32, .12), dtype=np.float32)
        base[:, 28:36] = (.28, .20, .12)
        row = dict(category="foliage / vegetation / alpha-cutout assets", assigned_material="grass",
                   asset_path="textures/foliage/tree.tga", route="alpha_material_reinterpretation")
        masks = PIPE.material_masks(row, base)
        self.assertLess(float(masks["grass"][8:56, 30:34].max()), .01)
        self.assertGreater(float(masks["grass"][8:56, 8:24].mean()), .5)

    def test_declared_character_metal_does_not_receive_cloth(self):
        base = np.full((64, 64, 3), (.20, .23, .17), dtype=np.float32)
        row = dict(category="character / uniform / skin textures", assigned_material="metal",
                   asset_path="skins/metal@helmet.dds", route="alpha_atlas_material_reinterpretation")
        masks = PIPE.material_masks(row, base)
        self.assertEqual(set(masks), {"metal"})
        self.assertEqual(float(masks["metal"].min()), 1)


if __name__ == "__main__":
    unittest.main()
