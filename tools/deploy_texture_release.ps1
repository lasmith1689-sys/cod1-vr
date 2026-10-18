[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [Parameter(Mandatory=$true)][string]$ReleaseRoot,
    [Parameter(Mandatory=$true)][string]$MountPlan,
    [string]$MainRoot = 'H:\SteamLibrary\steamapps\common\Call of Duty\Main'
)
# Texture-only deployment. WhatIf performs every admission check but writes
# nothing. Original archives are retained at the plan's literal disabled names.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Absolute([string]$path) {
    if (-not [IO.Path]::IsPathRooted($path)) { throw "Absolute path required: $path" }
    $value = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $cursor = $value
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse-point path is not admitted: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $value
}
function Child([string]$path, [string]$root) {
    $value = Absolute $path
    if ([IO.Path]::GetDirectoryName($value) -ine $root) { throw "Path is not a direct child of Main/package scope: $value" }
    return $value
}
function Hash([string]$path) {
    # Get-FileHash's PowerShell 5.1 internal member enumeration inherits WhatIf;
    # a read-only stream hash remains available during strict dry-run admission.
    $stream = [IO.File]::OpenRead($path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
function RequireHash([string]$path, [string]$expected) {
    if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or (Hash $path) -ine $expected) { throw "Archive hash mismatch: $path" }
}
function ReadJson([string]$path) { $value = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json; return $value }
function AssertNoGame {
    if (Get-Process -Name CoDSP,CoDMP -ErrorAction SilentlyContinue) {
        throw 'Close CoDSP/CoDMP before any deployment or rollback writes.'
    }
}
function IsImage([string]$name) { return [IO.Path]::GetExtension($name).ToLowerInvariant() -in @('.dds','.tga','.jpg','.jpeg','.png') }
function EntryName([string]$name) {
    $value = $name.Replace('\','/')
    if ($value.StartsWith('/') -or $value.Contains(':') -or $value.Split('/') -contains '..') { throw "Unsafe archive entry: $name" }
    return $value.ToLowerInvariant()
}
function ZipIndex([string]$path) {
    # Read the legacy ZIP central index, including CRCs; no raster decoding.
    # All admitted archives are <=900 MiB generated packs or existing legacy PK3s.
    $stream = [IO.File]::OpenRead($path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $length = [int][Math]::Min(65557, $stream.Length)
        [void]$stream.Seek(-$length, [IO.SeekOrigin]::End)
        $tail = $reader.ReadBytes($length)
        $end = -1
        for ($i = $tail.Length-22; $i -ge 0; $i--) {
            if ([BitConverter]::ToUInt32($tail,$i) -eq 0x06054b50 -and
                $i+22+[BitConverter]::ToUInt16($tail,$i+20) -eq $tail.Length) { $end=$i; break }
        }
        if ($end -lt 0) { throw "No valid ZIP central directory: $path" }
        $count = [BitConverter]::ToUInt16($tail,$end+10)
        $offset = [BitConverter]::ToUInt32($tail,$end+16)
        if ($count -eq 65535 -or $offset -eq [uint32]::MaxValue -or
            [BitConverter]::ToUInt16($tail,$end+4) -ne 0 -or [BitConverter]::ToUInt16($tail,$end+6) -ne 0) {
            throw "ZIP64/multidisk archive is not admitted: $path"
        }
        [void]$stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $index = @{}
        for ($i=0; $i -lt $count; $i++) {
            $header = $reader.ReadBytes(46)
            if ($header.Length -ne 46 -or [BitConverter]::ToUInt32($header,0) -ne 0x02014b50) { throw "Invalid ZIP central header: $path" }
            $nameLength = [BitConverter]::ToUInt16($header,28)
            $extraLength = [BitConverter]::ToUInt16($header,30)
            $commentLength = [BitConverter]::ToUInt16($header,32)
            $flags = [BitConverter]::ToUInt16($header,8)
            $encoding = if ($flags -band 2048) { [Text.Encoding]::UTF8 } else { [Text.Encoding]::GetEncoding(437) }
            $name = $encoding.GetString($reader.ReadBytes($nameLength))
            [void]$stream.Seek($extraLength+$commentLength,[IO.SeekOrigin]::Current)
            if ($name.EndsWith('/')) { continue }
            if ($flags -band 1) { throw "Encrypted ZIP entry is not admitted: $path $name" }
            $key = EntryName $name
            if ($index.ContainsKey($key)) { throw "Duplicate case-insensitive ZIP entry: $path $name" }
            $index[$key] = [pscustomobject]@{ name=$name; archive=$path; bytes=[BitConverter]::ToUInt32($header,24); crc32=[BitConverter]::ToUInt32($header,16) }
        }
        return $index
    } finally { $reader.Dispose(); $stream.Dispose() }
}
function EntryHash([string]$path, [string]$entry) {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        $found = $zip.GetEntry($entry)
        if ($null -eq $found) { throw "Missing entry: $path $entry" }
        $stream = $found.Open()
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
        finally { $algorithm.Dispose(); $stream.Dispose() }
    } finally { $zip.Dispose() }
}
function FileRecord([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return [ordered]@{path=$path; exists=$false} }
    $file = Get-Item -LiteralPath $path
    return [ordered]@{path=$path; exists=$true; bytes=$file.Length; sha256=(Hash $path)}
}
function AssertFileRecord($record) {
    if ([bool](Test-Path -LiteralPath $record.path -PathType Leaf) -ne [bool]$record.exists) { throw "Protected file existence changed: $($record.path)" }
    if ($record.exists -and ((Get-Item -LiteralPath $record.path).Length -ne $record.bytes -or (Hash $record.path) -ine $record.sha256)) {
        throw "Protected file bytes changed: $($record.path)"
    }
}

$ReleaseRoot = Absolute $ReleaseRoot
$MountPlan = Absolute $MountPlan
$MainRoot = Absolute $MainRoot
$liveMain = 'H:\SteamLibrary\steamapps\common\Call of Duty\Main'
$fixture = $MainRoot -ine $liveMain
if ($fixture) {
    $workspace = Absolute (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
    if (-not $WhatIfPreference -or -not $MainRoot.StartsWith($workspace+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Non-live MainRoot is permitted only for workspace fixtures with -WhatIf. Real writes require exact H: Main.'
    }
}
if ($ReleaseRoot -ieq $MainRoot -or $ReleaseRoot.StartsWith((Split-Path -Parent $MainRoot)+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'ReleaseRoot cannot overlap the installed game.'
}
$plan = ReadJson $MountPlan
if ((Absolute $plan.live_main) -ine $MainRoot) { throw 'Mount plan Main scope differs from requested MainRoot.' }
$coveragePath = Join-Path $ReleaseRoot 'coverage.json'
$manifestPath = Join-Path $ReleaseRoot 'manifest.json'
$packagePath = Join-Path $ReleaseRoot 'package.json'
$coverage = ReadJson $coveragePath
$manifest = @(ReadJson $manifestPath)
$package = ReadJson $packagePath
$expected = if ($fixture) { [int]$plan.runtime_assets.logical_images } else { 3654 }
if (-not $coverage.complete -or -not $coverage.release_ready -or $coverage.release_status -ne 'packaged_not_installed' -or
    $coverage.failed -ne 0 -or $coverage.logical_assets -ne $expected -or $coverage.source_manifest_assets -ne $expected -or
    $coverage.completed_assets -ne $expected -or -not $coverage.source_path_coverage.complete_logical_coverage -or
    $coverage.source_path_coverage.manifest_logical_assets -ne $expected -or $coverage.source_path_coverage.active_logical_assets -ne $expected -or
    $manifest.Count -ne $expected -or @(ReadJson (Join-Path $ReleaseRoot 'failures.json')).Count -ne 0) {
    throw 'Full complete PASS coverage and packaged_not_installed status are required.'
}
$ids = @{}; $expectedImages = @{}; $expectedAliases = @{}
foreach ($record in $manifest) {
    if ($record.validation -ne 'PASS' -or $ids.ContainsKey([string]$record.id)) { throw 'Every unique manifest record must PASS.' }
    $ids[[string]$record.id] = $true
    foreach ($source in $record.winning_sources) {
        $key = EntryName $source.entry
        if ($expectedAliases.ContainsKey($key)) { throw "Duplicate source alias: $key" }
        $expectedAliases[$key] = $source
    }
    if ($record.output_strategy -eq 'current_winning_bytes') {
        foreach ($entry in $record.output_entries) {
            $key = EntryName $entry.output_asset_path
            if ($expectedImages.ContainsKey($key)) { throw "Duplicate output alias: $key" }
            $expectedImages[$key] = [pscustomobject]@{sha256=$entry.sha256; source=$entry; preserved=$true}
        }
    } else {
        $key = EntryName $record.output_asset_path
        if ($expectedImages.ContainsKey($key)) { throw "Duplicate output alias: $key" }
        $expectedImages[$key] = [pscustomobject]@{sha256=$record.dds_sha256; source=$null; preserved=$false}
    }
}
if ($expectedAliases.Count -ne $plan.runtime_assets.physical_image_aliases -or
    $package.alias_coverage.logical_assets -ne $expected -or @($package.alias_coverage.missing_preserved_aliases).Count -ne 0 -or
    @($package.alias_coverage.missing_logical_assets).Count -ne 0 -or @($package.alias_coverage.extra_logical_assets).Count -ne 0) {
    throw 'Logical/physical alias coverage is incomplete.'
}

$retiring = @($plan.retire_archives)
$stocks = @($plan.stock_archive_records)
if ($retiring.Count -ne $plan.admission_gates.retire_count -or $stocks.Count -ne @($plan.stock_archives_untouched).Count -or
    (-not $fixture -and ($retiring.Count -ne 11 -or $stocks.Count -ne 16 -or $plan.nonimage_summary.physical_entries -ne 69 -or
     $plan.admission_gates.retention_archive_count -ne 6))) { throw 'Sealed stock/retirement/retention counts differ.' }
$archiveRecords = @{}; $indexes = @{}; $stockProof = @(); $liveWinners = @{}
foreach ($source in @($stocks)+@($retiring)) {
    $path = Child $source.live_source $MainRoot
    if ([IO.Path]::GetFileName($path) -ine $source.archive -or $archiveRecords.ContainsKey($source.archive)) { throw 'Duplicate or mismatched source archive.' }
    $file = Get-Item -LiteralPath $path
    $time = [DateTimeOffset]::Parse($source.last_write_utc).UtcDateTime
    if ($file.Length -ne $source.archive_bytes -or [Math]::Abs($file.LastWriteTimeUtc.Ticks-$time.Ticks) -gt 10) { throw "Source archive stat changed: $path" }
    RequireHash $path $source.archive_sha256
    $archiveRecords[$source.archive] = $source
    $indexes[$path] = ZipIndex $path
}
$activeNames = @(Get-ChildItem -LiteralPath $MainRoot -File | Where-Object { $_.Extension -ieq '.pk3' } | ForEach-Object { $_.Name })
if ($activeNames.Count -ne $archiveRecords.Count -or @($activeNames | Where-Object { -not $archiveRecords.ContainsKey($_) }).Count) {
    throw 'Unplanned active PK3 collision: baseline mount set differs from sealed plan.'
}
foreach ($name in @($archiveRecords.Keys | Sort-Object)) {
    foreach ($key in $indexes[$archiveRecords[$name].live_source].Keys) { $liveWinners[$key] = $indexes[$archiveRecords[$name].live_source][$key] }
}
$liveImages = @($liveWinners.Keys | Where-Object { IsImage $_ })
if ($liveImages.Count -ne $expectedAliases.Count) { throw 'Current winning physical-image set changed.' }
foreach ($key in $expectedAliases.Keys) {
    $source = $expectedAliases[$key]
    if (-not $liveWinners.ContainsKey($key) -or $liveWinners[$key].archive -ine (Absolute $source.archive) -or
        $liveWinners[$key].bytes -ne $source.bytes -or $liveWinners[$key].crc32 -ne [uint32]$source.crc32) {
        throw "Current winning alias/CRC changed: $key"
    }
}
foreach ($key in $expectedImages.Keys) {
    $entry = $expectedImages[$key]
    if ($entry.preserved -and (EntryHash $entry.source.archive $entry.source.entry) -ine $entry.sha256) { throw "Current preserved alias hash changed: $key" }
}
foreach ($stock in $stocks) {
    if ($stock.archive -notin $plan.stock_archives_untouched) { throw 'Stock archive is not explicitly untouched.' }
    $stockProof += FileRecord $stock.live_source
}
$disableTargets = @{}
foreach ($old in $retiring) {
    if ($old.archive -in $plan.stock_archives_untouched) { throw 'A stock archive may not be retired.' }
    $target = Child $old.retire_target $MainRoot
    if (-not [IO.Path]::GetFileName($target).StartsWith($old.archive+'.disabled-texture-reinterpret-',[StringComparison]::OrdinalIgnoreCase)) { throw 'Retirement name differs from recoverable plan.' }
    if ((Test-Path -LiteralPath $target) -or $disableTargets.ContainsKey($target)) { throw "Retirement target collision: $target" }
    $disableTargets[$target] = $true
}

$packagesDir = Absolute (Join-Path $ReleaseRoot 'packages')
$install = @(); $newNames = @{}; $newEntries = @{}; $nonimageCount = 0
$nonce = Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'
foreach ($image in @($package.images)) {
    $path = Child $image.path $packagesDir
    $name = [IO.Path]::GetFileName($path)
    if ($name -notmatch '^zzzzvreinterpret\d{2}\.pk3$') { throw "Unexpected image package name: $name" }
    RequireHash $path $image.sha256
    $index = ZipIndex $path
    if ($index.Count -ne $image.images) { throw 'Image package count mismatch.' }
    foreach ($key in $index.Keys) {
        if (-not (IsImage $key) -or -not $expectedImages.ContainsKey($key) -or $newEntries.ContainsKey($key)) { throw "Unexpected/duplicate image package entry: $key" }
        if ((EntryHash $path $index[$key].name) -ine $expectedImages[$key].sha256) { throw "Image entry hash mismatch: $key" }
        $newEntries[$key] = $index[$key]
    }
    $install += [pscustomobject]@{source=$path; target=(Child (Join-Path $MainRoot $name) $MainRoot); sha256=$image.sha256; kind='images'}
}
if (@($package.images).Count -eq 0 -or $newEntries.Count -ne $expectedImages.Count) { throw 'Packaged image aliases are incomplete.' }
$retention = @($package.optional_nonimage_retention)
if ($retention.Count -ne $plan.admission_gates.retention_archive_count) { throw 'Nonimage retention archive count differs.' }
$retentionSources = @{}
foreach ($keep in $retention) {
    $sourcePath = Child $keep.source $MainRoot
    $old = @($retiring | Where-Object { $_.live_source -ieq $sourcePath })
    if ($old.Count -ne 1 -or -not $keep.entry_bytes_preserved -or $retentionSources.ContainsKey($sourcePath)) { throw 'Unplanned/duplicate nonimage retention source.' }
    $retentionSources[$sourcePath]=$true
    $path = Child $keep.retained_archive $packagesDir
    if ([IO.Path]::GetFileName($path) -ine $old[0].nonimage_retention_archive) { throw 'Nonimage retention archive name differs.' }
    RequireHash $path $keep.sha256
    $index = ZipIndex $path
    $expectedNonimages = @($old[0].nonimage_entry_records)
    if ($index.Count -ne $expectedNonimages.Count -or $index.Count -ne $keep.entries) { throw 'Nonimage retention entry count differs.' }
    foreach ($entry in $expectedNonimages) {
        $key = EntryName $entry.entry
        if ((IsImage $key) -or -not $entry.still_current_winner -or -not $index.ContainsKey($key) -or
            -not $liveWinners.ContainsKey($key) -or $liveWinners[$key].archive -ine $sourcePath -or
            $index[$key].bytes -ne $entry.bytes -or (EntryHash $path $index[$key].name) -ine $entry.sha256 -or
            (EntryHash $sourcePath $liveWinners[$key].name) -ine $entry.sha256) { throw "Nonimage current-winner bytes/hash mismatch: $key" }
        if ($newEntries.ContainsKey($key)) { throw "Duplicate new nonimage entry: $key" }
        $newEntries[$key]=$index[$key]; $nonimageCount++
    }
    $install += [pscustomobject]@{source=$path; target=(Child (Join-Path $MainRoot ([IO.Path]::GetFileName($path))) $MainRoot); sha256=$keep.sha256; kind='nonimages'}
}
if ($nonimageCount -ne $plan.nonimage_summary.physical_entries -or -not $plan.nonimage_summary.all_retained_entries_are_current_winners) {
    throw 'All planned nonimage current winners must be retained exactly.'
}
foreach ($new in $install) {
    if ((Test-Path -LiteralPath $new.target) -or $newNames.ContainsKey($new.target) -or $archiveRecords.ContainsKey([IO.Path]::GetFileName($new.target))) { throw "New archive target collision: $($new.target)" }
    $newNames[$new.target]=$true
    Add-Member -InputObject $new -NotePropertyName failed_target -NotePropertyValue (Child ($new.target+'.disabled-failed-'+$nonce) $MainRoot)
    if (Test-Path -LiteralPath $new.failed_target) { throw "Rollback target collision: $($new.failed_target)" }
}
# Check the hypothetical full mount, not merely individual package contents.
$future = @{}
$futureArchives = @()
foreach ($stock in $stocks) { $futureArchives += [pscustomobject]@{name=$stock.archive; index=$indexes[$stock.live_source]} }
foreach ($new in $install) { $futureArchives += [pscustomobject]@{name=[IO.Path]::GetFileName($new.target); index=(ZipIndex $new.source)} }
foreach ($archive in @($futureArchives | Sort-Object name)) { foreach ($key in $archive.index.Keys) { $future[$key]=$archive.index[$key] } }
foreach ($key in $newEntries.Keys) {
    if (-not $future.ContainsKey($key) -or $future[$key].archive -ine $newEntries[$key].archive) { throw "New image/nonimage does not win after mounting: $key" }
}
foreach ($key in $liveWinners.Keys) {
    if (IsImage $key) { continue }
    if (-not $future.ContainsKey($key)) { throw "Current nonimage winner would be lost: $key" }
    if ($future[$key].archive -ine $liveWinners[$key].archive -and
        (EntryHash $future[$key].archive $future[$key].name) -ine (EntryHash $liveWinners[$key].archive $liveWinners[$key].name)) {
        throw "Current nonimage winner bytes would change: $key"
    }
}
$gameRoot = Split-Path -Parent $MainRoot
$protectedProof = @()
foreach ($relative in @('opengl32.dll','openxr_loader.dll','codvr.cfg','ReShade.ini','ReShadeVR.ini','CODVR_Remastered.ini','CODVR_NativePostprocess.ini','Main\autoexec.cfg','Main\config.cfg')) {
    $protectedProof += FileRecord (Absolute (Join-Path $gameRoot $relative))
}
if ($WhatIfPreference) {
    Write-Output ("DRY RUN PASS: {0} logical textures; {1} explicit new archives; {2} recoverable retirements; {3} exact nonimages. No files changed." -f $expected,$install.Count,$retiring.Count,$nonimageCount)
    return
}
AssertNoGame
if (-not $PSCmdlet.ShouldProcess($MainRoot,'Copy verified texture archives and reversibly disable only the sealed old archives')) { return }
$receiptDir = Join-Path $ReleaseRoot 'deploy_receipts'
[void](New-Item -ItemType Directory -Path $receiptDir -Force)
$receiptPath = Join-Path $receiptDir ('texture-deploy-'+$nonce+'.json')
$receipt = [ordered]@{schema='codvr-texture-deploy-1'; status='prepared'; main=$MainRoot; release_root=$ReleaseRoot;
    mount_plan=$MountPlan; mount_plan_sha256=(Hash $MountPlan); package_sha256=(Hash $packagePath); manifest_sha256=(Hash $manifestPath);
    source_archives=@($stocks)+@($retiring); planned_install=$install; stock_proof=$stockProof; dll_config_proof=$protectedProof;
    operations=@(); installed_hashes=@(); retained_nonimages=$nonimageCount; error=$null; rollback_errors=@(); receipt_errors=@()}
function SaveReceipt([switch]$BestEffort) {
    try {
        $temporary = $receiptPath+'.tmp'
        $receipt | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $temporary -Encoding UTF8
        if (Test-Path -LiteralPath $receiptPath) {
            # Atomic journal replacement preserves every preceding complete
            # receipt; no original archive or partially written pack is deleted.
            [IO.File]::Replace($temporary,$receiptPath,($receiptPath+'.previous-'+[guid]::NewGuid().ToString('N')))
        } else { [IO.File]::Move($temporary,$receiptPath) }
    } catch {
        if (-not $BestEffort) { throw }
        $receipt.receipt_errors += $_.Exception.Message
        Write-Warning "Receipt update failed; rollback continues independently: $($_.Exception.Message)" -WarningAction Continue
    }
}
SaveReceipt # Baseline hashes/paths are recorded before the first live mutation.
$copied = @(); $moved = @()
try {
    # Originals remain mounted until every new archive is copied and hash-checked.
    foreach ($new in @($install | Sort-Object kind,target)) {
        AssertNoGame
        RequireHash $new.source $new.sha256
        if (Test-Path -LiteralPath $new.target) { throw "Late target collision: $($new.target)" }
        $copied += $new # Include a partial Copy-Item in rollback ownership.
        Copy-Item -LiteralPath $new.source -Destination $new.target
        RequireHash $new.target $new.sha256
        $receipt.operations += [ordered]@{action='copy_verified'; source=$new.source; target=$new.target; sha256=(Hash $new.target)}
        SaveReceipt
    }
    foreach ($old in $retiring) {
        AssertNoGame
        [void](Child $old.live_source $MainRoot); [void](Child $old.retire_target $MainRoot)
        RequireHash $old.live_source $old.archive_sha256
        if (Test-Path -LiteralPath $old.retire_target) { throw "Late retirement collision: $($old.retire_target)" }
        $moved += $old # Track ownership before a move that may report an error after renaming.
        Move-Item -LiteralPath $old.live_source -Destination $old.retire_target
        RequireHash $old.retire_target $old.archive_sha256
        $receipt.operations += [ordered]@{action='disable_recoverable'; source=$old.live_source; target=$old.retire_target; sha256=$old.archive_sha256}
        SaveReceipt
    }
    foreach ($proof in @($stockProof)+@($protectedProof)) { AssertFileRecord $proof }
    foreach ($new in $install) { RequireHash $new.target $new.sha256; $receipt.installed_hashes += FileRecord $new.target }
    $receipt.status='installed_texture_only'; SaveReceipt
    Write-Output "Texture-only deployment verified. Receipt: $receiptPath. Next: stock game startup and representative in-game/VR texture checks in the current thread."
} catch {
    $receipt.error=$_.Exception.Message; $receipt.status='rollback_started'; SaveReceipt -BestEffort
    # Never delete partial/new archives. Keep them recoverable under explicit
    # disabled-failed names, then restore only this run's original archives.
    foreach ($new in @($copied | Sort-Object target -Descending)) {
        try {
            AssertNoGame
            [void](Child $new.target $MainRoot); [void](Child $new.failed_target $MainRoot)
            if (Test-Path -LiteralPath $new.target) {
                if (Test-Path -LiteralPath $new.failed_target) { throw "Rollback collision: $($new.failed_target)" }
                Move-Item -LiteralPath $new.target -Destination $new.failed_target
                $receipt.operations += [ordered]@{action='rollback_disable_new'; source=$new.target; target=$new.failed_target}
            }
        } catch { $receipt.rollback_errors += $_.Exception.Message }
    }
    foreach ($old in $moved) {
        try {
            AssertNoGame
            [void](Child $old.retire_target $MainRoot); [void](Child $old.live_source $MainRoot)
            if (-not (Test-Path -LiteralPath $old.retire_target)) {
                # The attempted move may have failed before changing the file.
                RequireHash $old.live_source $old.archive_sha256
                continue
            }
            if (Test-Path -LiteralPath $old.live_source) { throw "Rollback original-name collision: $($old.live_source)" }
            RequireHash $old.retire_target $old.archive_sha256
            Move-Item -LiteralPath $old.retire_target -Destination $old.live_source
            RequireHash $old.live_source $old.archive_sha256
            $receipt.operations += [ordered]@{action='rollback_restore_original'; source=$old.retire_target; target=$old.live_source}
        } catch { $receipt.rollback_errors += $_.Exception.Message }
    }
    $receipt.status = if ($receipt.rollback_errors.Count) { 'rollback_requires_attention' } else { 'rolled_back_recoverably' }
    SaveReceipt -BestEffort
    throw "Texture deployment failed: $($receipt.error). Status=$($receipt.status). Receipt=$receiptPath. Receipt journaling errors=$($receipt.receipt_errors.Count)."
}
