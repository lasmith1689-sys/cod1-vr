"""PowerShell deployment admission fixtures: WhatIf only, never live writes."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile
import zlib


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "deploy_texture_release.ps1"
POWERSHELL = Path(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class TextureDeployTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "Texture-only deployment helper is not implemented")
        self.temp = tempfile.TemporaryDirectory(prefix="deploy-fixture-", dir=ROOT / "evidence")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.main = self.base / "game" / "Main"
        self.main.mkdir(parents=True)
        self.release = self.base / "release"
        (self.release / "packages").mkdir(parents=True)
        self.plan_path = self.base / "mount.json"
        stock = self.main / "pak0.pk3"
        old = self.main / "oldart.pk3"
        assets = [(stock, "textures/stock.tga", b"stock preserved bytes"),
                  (old, "textures/old.tga", b"current winning bytes")]
        nonimage = b"unchanged nonimage resource"
        with zipfile.ZipFile(stock, "w") as archive:
            archive.writestr(assets[0][1], assets[0][2])
        with zipfile.ZipFile(old, "w") as archive:
            archive.writestr(assets[1][1], assets[1][2])
            archive.writestr("metadata/receipt.json", nonimage)
        records = []
        for index, (path, name, data) in enumerate(assets, 1):
            source = {"archive": str(path), "entry": name, "bytes": len(data), "crc32": zlib.crc32(data)}
            records.append({"id": f"{index:04}", "asset_path": name, "validation": "PASS",
                            "route": "faithful_ui_preservation", "output_strategy": "current_winning_bytes",
                            "winning_sources": [source], "output_entries": [dict(source, output_asset_path=name,
                                sha256=hashlib.sha256(data).hexdigest())]})
        image = self.release / "packages" / "zzzzvreinterpret01.pk3"
        with zipfile.ZipFile(image, "w") as archive:
            for _, name, data in assets:
                archive.writestr(name, data)
        retention = self.release / "packages" / "oldart_nonimages.pk3"
        with zipfile.ZipFile(retention, "w") as archive:
            archive.writestr("metadata/receipt.json", nonimage)
        self.coverage = {"logical_assets": 2, "source_manifest_assets": 2, "completed_assets": 2,
                         "complete": True, "failed": 0, "release_ready": True,
                         "release_status": "packaged_not_installed", "source_path_coverage": {
                             "complete_logical_coverage": True, "manifest_logical_assets": 2,
                             "active_logical_assets": 2, "active_physical_aliases": 2}}
        self.package = {"images": [{"path": str(image), "images": 2, "sha256": digest(image)}],
                        "optional_nonimage_retention": [{"source": str(old), "retained_archive": str(retention),
                            "entries": 1, "sha256": digest(retention), "entry_bytes_preserved": True}],
                        "alias_coverage": {"logical_assets": 2, "missing_preserved_aliases": [],
                            "missing_logical_assets": [], "extra_logical_assets": []}}

        def stat(path):
            return {"archive": path.name, "live_source": str(path), "archive_bytes": path.stat().st_size,
                    "last_write_utc": datetime.fromtimestamp(path.stat().st_mtime, timezone.utc).isoformat(),
                    "archive_sha256": digest(path)}

        retire = dict(stat(old), retire_target=str(old) + ".disabled-texture-reinterpret-20261005",
                      nonimage_retention_archive=retention.name,
                      nonimage_entry_records=[{"entry": "metadata/receipt.json", "bytes": len(nonimage),
                          "crc32": f"{zlib.crc32(nonimage):08x}", "sha256": hashlib.sha256(nonimage).hexdigest(),
                          "current_winning_archive": old.name, "still_current_winner": True}])
        self.plan = {"schema": "codvr-texture-mount-plan-1", "live_main": str(self.main),
                     "runtime_assets": {"logical_images": 2, "physical_image_aliases": 2},
                     "stock_archives_untouched": [stock.name], "stock_archive_records": [stat(stock)],
                     "retire_archives": [retire], "nonimage_summary": {"physical_entries": 1,
                         "all_retained_entries_are_current_winners": True},
                     "admission_gates": {"retire_count": 1, "retention_archive_count": 1}}
        self.write("coverage.json", self.coverage)
        self.write("package.json", self.package)
        self.write("manifest.json", records)
        self.write("failures.json", [])
        self.plan_path.write_text(json.dumps(self.plan))

    def write(self, name, data):
        (self.release / name).write_text(json.dumps(data))

    def run_script(self, *, what_if=True, extra=()):
        command = [str(POWERSHELL), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
                   "-ReleaseRoot", str(self.release), "-MountPlan", str(self.plan_path),
                   "-MainRoot", str(self.main), *extra]
        if what_if:
            command.append("-WhatIf")
        return subprocess.run(command, capture_output=True, text=True, timeout=40)

    def test_valid_dry_run_checks_archive_bytes_without_any_mutation(self):
        before = {str(path): digest(path) for path in self.base.rglob("*") if path.is_file()}
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DRY RUN", result.stdout)
        after = {str(path): digest(path) for path in self.base.rglob("*") if path.is_file()}
        self.assertEqual(before, after)

    def test_colliding_new_target_rejects_before_any_move(self):
        collision = self.main / "zzzzvreinterpret01.pk3"
        collision.write_bytes(b"existing unrelated archive")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("collision", result.stdout + result.stderr)
        self.assertEqual(collision.read_bytes(), b"existing unrelated archive")
        self.assertTrue((self.main / "oldart.pk3").exists())

    def test_package_hash_mismatch_rejects_before_any_move(self):
        self.package["images"][0]["sha256"] = "0" * 64
        self.write("package.json", self.package)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hash", result.stdout + result.stderr)
        self.assertTrue((self.main / "oldart.pk3").exists())

    def test_path_escape_and_fixture_real_write_are_rejected(self):
        self.plan["retire_archives"][0]["retire_target"] = str(self.main.parent / "escape.pk3.disabled")
        self.plan_path.write_text(json.dumps(self.plan))
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Main", result.stdout + result.stderr)
        result = self.run_script(what_if=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("WhatIf", result.stdout + result.stderr)
        self.assertFalse((self.main.parent / "escape.pk3.disabled").exists())

    def test_incomplete_release_or_nonimage_change_blocks_mount(self):
        self.coverage["complete"] = False
        self.write("coverage.json", self.coverage)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("complete", result.stdout + result.stderr)
        self.coverage["complete"] = True
        self.write("coverage.json", self.coverage)
        self.plan["retire_archives"][0]["nonimage_entry_records"][0]["sha256"] = "f" * 64
        self.plan_path.write_text(json.dumps(self.plan))
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nonimage", (result.stdout + result.stderr).lower())
        self.assertTrue((self.main / "oldart.pk3").exists())

    def test_copy_move_and_receipt_failures_restore_originals_recoverably(self):
        # Execute the real script's deployment try/catch AST with filesystem
        # cmdlets mapped to harmless workspace files. Its MainRoot remains the
        # exact live scope; no production gate or process check is bypassed.
        for failure in ("copy", "move", "move_after", "receipt"):
            with self.subTest(failure=failure):
                fixture = self.base / failure
                fixture.mkdir()
                (fixture / "candidate.pk3").write_bytes(b"new candidate")
                (fixture / "old-a.pk3").write_bytes(b"original A")
                (fixture / "old-b.pk3").write_bytes(b"original B")
                code = r'''
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile('__SCRIPT__',[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Script parse failed' }
foreach ($function in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$body=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.TryStatementAst] -and $n.Extent.Text.Contains('Texture deployment failed:')},$true))
if ($body.Count -ne 1) { throw 'Deployment recovery body not found' }
$MainRoot='H:\SteamLibrary\steamapps\common\Call of Duty\Main'
$fixture='__FIXTURE__'; $mode='__FAILURE__'
$oldA=Join-Path $MainRoot 'unit-original-a.pk3'; $oldB=Join-Path $MainRoot 'unit-original-b.pk3'
$newPath=Join-Path $MainRoot 'unit-candidate.pk3'
$map=@{}
$map[$oldA]=Join-Path $fixture 'old-a.pk3'; $map[$oldB]=Join-Path $fixture 'old-b.pk3'
$map[$oldA+'.disabled']=Join-Path $fixture 'old-a.disabled'; $map[$oldB+'.disabled']=Join-Path $fixture 'old-b.disabled'
$map[$newPath]=Join-Path $fixture 'new.pk3'; $map[$newPath+'.disabled-failed']=Join-Path $fixture 'new.disabled-failed'
function Mapped([string]$path) {
    if ($map.ContainsKey($path)) { return $map[$path] }
    if ($path.StartsWith('H:',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unmapped live write rejected by test' }
    return $path
}
function Test-Path([string]$LiteralPath,[string]$PathType) {
    if ($map.ContainsKey($LiteralPath)) { $LiteralPath=$map[$LiteralPath] }
    if ($PathType) { return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath
}
function Get-Item([string]$LiteralPath) {
    if ($map.ContainsKey($LiteralPath)) { $LiteralPath=$map[$LiteralPath] }
    return Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath
}
function Hash([string]$path) {
    $stream=[IO.File]::OpenRead((Mapped $path)); $algorithm=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
$script:copyFailed=$false; $script:moveFailed=$false; $script:receiptWrites=0
function Copy-Item([string]$LiteralPath,[string]$Destination) {
    if ($mode -eq 'copy' -and -not $script:copyFailed) {
        $script:copyFailed=$true; [IO.File]::WriteAllText((Mapped $Destination),'partial candidate')
        throw 'Injected copy failure'
    }
    Microsoft.PowerShell.Management\Copy-Item -LiteralPath (Mapped $LiteralPath) -Destination (Mapped $Destination)
}
function Move-Item([string]$LiteralPath,[string]$Destination) {
    if ($mode -in @('move','move_after') -and $LiteralPath -eq $oldB -and -not $script:moveFailed) {
        $script:moveFailed=$true
        if ($mode -eq 'move_after') {
            Microsoft.PowerShell.Management\Move-Item -LiteralPath (Mapped $LiteralPath) -Destination (Mapped $Destination)
        }
        throw 'Injected move failure'
    }
    Microsoft.PowerShell.Management\Move-Item -LiteralPath (Mapped $LiteralPath) -Destination (Mapped $Destination)
}
function Set-Content([string]$LiteralPath,[string]$Encoding,[Parameter(ValueFromPipeline=$true)]$Value) {
    process {
        $script:receiptWrites++
        if ($mode -eq 'receipt' -and $script:receiptWrites -ge 2) { throw 'Injected receipt write failure' }
        Microsoft.PowerShell.Management\Set-Content -LiteralPath (Mapped $LiteralPath) -Value $Value -Encoding $Encoding
    }
}
$candidate=Join-Path $fixture 'candidate.pk3'
$install=@([pscustomobject]@{source=$candidate;target=$newPath;failed_target=$newPath+'.disabled-failed';sha256=(Hash $candidate);kind='images'})
$retiring=@([pscustomobject]@{live_source=$oldA;retire_target=$oldA+'.disabled';archive_sha256=(Hash $oldA)},
            [pscustomobject]@{live_source=$oldB;retire_target=$oldB+'.disabled';archive_sha256=(Hash $oldB)})
$stockProof=@();$protectedProof=@();$receiptPath=Join-Path $fixture 'receipt.json'
$receipt=[ordered]@{status='prepared';operations=@();installed_hashes=@();error=$null;rollback_errors=@();receipt_errors=@()}
$copied=@();$moved=@()
SaveReceipt
try { . ([scriptblock]::Create($body[0].Extent.Text)) } catch { Write-Output $_.Exception.Message }
if ($receipt.status -ne 'rolled_back_recoverably') { throw ('Rollback did not finish: '+$receipt.status) }
if (-not (Test-Path -LiteralPath $oldA) -or -not (Test-Path -LiteralPath $oldB) -or
    (Test-Path -LiteralPath $newPath) -or -not (Test-Path -LiteralPath ($newPath+'.disabled-failed'))) { throw 'Recovery file state mismatch' }
if ((Get-Content -LiteralPath (Mapped $oldA) -Raw) -ne 'original A' -or
    (Get-Content -LiteralPath (Mapped $oldB) -Raw) -ne 'original B') { throw 'Original bytes changed' }
Write-Output 'RECOVERY PASS'
'''.replace("__SCRIPT__", str(SCRIPT)).replace("__FIXTURE__", str(fixture)).replace("__FAILURE__", failure)
                result = subprocess.run([str(POWERSHELL), "-NoProfile", "-Command", code], capture_output=True, text=True, timeout=40)
                if "Close CoDSP/CoDMP" in result.stdout + result.stderr:
                    self.skipTest("Recovery simulation honors the stopped-game gate")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("RECOVERY PASS", result.stdout)
                if failure == "receipt":
                    self.assertIn("receipt", (result.stdout + result.stderr).lower())


if __name__ == "__main__":
    unittest.main()
