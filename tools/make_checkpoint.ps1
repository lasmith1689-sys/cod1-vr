<#
  Immutable source/build checkpoint for a candidate (review 2026-08-29, §12).

  Copies the complete relevant source state -- src/ (including the generated
  ownership and landmark tables), tests/, tools/*.py and *.ps1, build.ps1,
  docs/*.md -- plus the gated dist DLL and cfg into
  checkpoints/candidate-<DLLHASH8>-<date>/ and writes a SHA256 manifest over
  every file. Refuses to overwrite an existing checkpoint directory.

  Usage: .\tools\make_checkpoint.ps1 [-Label <extra>]
#>
param([string]$Label = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll  = Join-Path $root 'dist\opengl32.dll'
$cfg  = Join-Path $root 'dist\codvr.cfg'
if (-not (Test-Path $dll)) { throw "no dist DLL at $dll" }
if (-not (Test-Path $cfg)) { throw "no dist cfg at $cfg" }

$hash8 = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8)
$date  = Get-Date -Format 'yyyy-MM-dd'
$name  = "candidate-$hash8-$date"
if ($Label) { $name = "$name-$Label" }
$dst = Join-Path $root "checkpoints\$name"
if (Test-Path $dst) { throw "checkpoint already exists: $dst (checkpoints are immutable)" }
New-Item -ItemType Directory -Path $dst | Out-Null

$sets = @(
    @{ src = 'src';   filter = '*';      sub = 'src'   },
    @{ src = 'tests'; filter = '*';      sub = 'tests' },
    @{ src = 'tools'; filter = '*.py';   sub = 'tools' },
    @{ src = 'tools'; filter = '*.ps1';  sub = 'tools' },
    @{ src = 'tools'; filter = '*.cpp';  sub = 'tools' },
    @{ src = 'docs';  filter = '*.md';   sub = 'docs'  },
    @{ src = 'docs';  filter = '*.json'; sub = 'docs'  }
)
foreach ($s in $sets) {
    $from = Join-Path $root $s.src
    $to   = Join-Path $dst $s.sub
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Get-ChildItem -Path $from -File -Filter $s.filter | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $to $_.Name)
    }
}
Copy-Item (Join-Path $root 'build.ps1') (Join-Path $dst 'build.ps1')
Copy-Item $dll (Join-Path $dst 'opengl32.dll')
Copy-Item $cfg (Join-Path $dst 'codvr.cfg')
if (Test-Path (Join-Path $root 'dist\deploy-provenance.json')) {
    Copy-Item (Join-Path $root 'dist\deploy-provenance.json') (Join-Path $dst 'deploy-provenance.json')
}

$manifest = Join-Path $dst 'SHA256SUMS.txt'
$lines = Get-ChildItem -Path $dst -File -Recurse | Where-Object { $_.FullName -ne $manifest } |
    Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($dst.Length + 1)
        "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)  $rel"
    }
$lines | Set-Content $manifest -Encoding ascii
Write-Host "checkpoint: $dst"
Write-Host "  files    : $($lines.Count)"
Write-Host "  dll      : $((Get-FileHash $dll -Algorithm SHA256).Hash)"
Write-Host "  cfg      : $((Get-FileHash $cfg -Algorithm SHA256).Hash)"
Write-Host "  manifest : $manifest"
