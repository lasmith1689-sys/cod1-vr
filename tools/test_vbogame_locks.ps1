<#
  Self-test for vbogame_run.ps1's lock handling -- reproduces the exact
  failure class that burned the 2026-08-27 noon attempts ("opengl32.dll is
  being used by another process", owner never named, no retry), WITHOUT the
  user, WITHOUT Steam, WITHOUT the real game folder.

  Each case drives the REAL one-command harness (-GameDirTest sandbox mode:
  same code path through pre-flight, lock gate, staged deploy, verification
  and finally-restore; only Steam launch and artefact collection are
  skipped) against a sandbox game dir seeded with the true safe-baseline
  bytes, while a child process holds the sandbox DLL open the way a loaded
  module does (readable, not writable).

  Cases (matching the requirement list):
    A  no lock                          -> deploy + restore succeed (exit 0)
    B  lock held, owner NOT safe-listed -> owner named (pid+name), abort...
    D  ...BEFORE any mutation           -> exit 2
    E  ...and the DLL is byte-identical to the baseline
    C  lock held, owner safe-listed     -> harness clears it, succeeds (exit 0)
    F  restore meets a 5 s lock         -> bounded retry wins (exit 0)
    G  restore lock outlives the budget -> exit 3 + fatal report + backup kept
    H  every case: exit 0 <=> final DLL hash == pre-run hash
#>
param([switch]$KeepSandbox)

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$harness = Join-Path $PSScriptRoot 'vbogame_run.ps1'
$ckpt    = Join-Path $root 'checkpoints\deployed-as-found-2026-08-24'
$distDll = Join-Path $root 'dist\opengl32.dll'
$SAFE    = 'B3705FE51711C0DCA623159C6317A79CD4640BB7B4583E3B6345165182249CC6'
$sand    = Join-Path $root 'runs\locktest-sandbox'
$INSTR   = (Get-FileHash $distDll).Hash

$pass = 0; $fail = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  PASS  $name" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "  FAIL  $name" -ForegroundColor Red;   $script:fail++ }
}

function Reset-Sandbox {
    if (Test-Path $sand) { Remove-Item $sand -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $sand, (Join-Path $sand 'Main') | Out-Null
    Copy-Item (Join-Path $ckpt 'opengl32.dll') (Join-Path $sand 'opengl32.dll')
    Copy-Item (Join-Path $ckpt 'codvr.cfg')    (Join-Path $sand 'codvr.cfg')
}

# A child that holds the file the way a loaded module does: readable by
# others, NOT writable. It releases when $release appears or after $holdSec.
function Start-Locker([string]$path, [int]$holdSec, [string]$release) {
    $script = "`$fs=[IO.File]::Open('$path','Open','Read','Read');" +
              "`$sw=[Diagnostics.Stopwatch]::StartNew();" +
              "while(`$sw.Elapsed.TotalSeconds -lt $holdSec -and -not (Test-Path '$release')){Start-Sleep -Milliseconds 200};" +
              "`$fs.Close()"
    return Start-Process powershell -ArgumentList '-NoProfile','-Command',$script -PassThru -WindowStyle Hidden
}

function Run-Harness([string[]]$extra) {
    $args_ = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$harness,
               '-SkipBuild','-GameDirTest',$sand) + $extra
    $out = & powershell @args_ 2>&1 | Out-String
    return @{ Exit = $LASTEXITCODE; Out = $out }
}

function SandDllSha { (Get-FileHash (Join-Path $sand 'opengl32.dll')).Hash }

Write-Host "=== vbogame lock-handling self-test ===" -ForegroundColor Cyan
Write-Host "  sandbox: $sand"
Write-Host "  safe baseline $($SAFE.Substring(0,8))..., instrumented $($INSTR.Substring(0,8))..."

# ---- A: no lock -> the whole transaction succeeds and restores -------------
Reset-Sandbox
$r = Run-Harness @()
Check ($r.Exit -eq 0) "A: clean sandbox -> exit 0"
Check ($r.Out -match 'probe deployed and hash verified') "A: deploy actually happened and was verified"
Check ($r.Out -match 'RESTORED and verified') "A: restore verified"
Check ((SandDllSha) -eq $SAFE) "A/H: final DLL is the pre-run baseline"

# ---- B/D/E: unknown owner -> named, abort pre-mutation, no bytes change ----
Reset-Sandbox
$rel = Join-Path $sand 'release-B'
$lk = Start-Locker (Join-Path $sand 'opengl32.dll') 120 $rel
Start-Sleep -Seconds 1
$r = Run-Harness @('-LockWaitSec','8')
New-Item $rel -ItemType File -Force | Out-Null
$lk.WaitForExit()
Check ($r.Exit -eq 2) "B/D: unknown lock owner -> abort, exit 2"
Check ($r.Out -match "lock owner: pid $($lk.Id) 'powershell'") "B: the exact owning pid+name was reported"
Check ($r.Out -match 'UNKNOWN process holds') "D: abort names the unknown-owner cause"
Check ($r.Out -notmatch 'probe deployed') "D: no deployment was attempted"
Check ((SandDllSha) -eq $SAFE) "E/H: DLL byte-identical after pre-mutation abort"
Check ((Get-ChildItem $sand -Filter '*.vbogame_*').Count -eq 0) "E: no stage/backup litter left behind"

# ---- C: safe-listed transient owner -> cleared automatically, success ------
Reset-Sandbox
$rel = Join-Path $sand 'release-C'
$lk = Start-Locker (Join-Path $sand 'opengl32.dll') 120 $rel
Start-Sleep -Seconds 1
$r = Run-Harness @('-SafeLockOwners','powershell','-LockWaitSec','30')
New-Item $rel -ItemType File -Force -EA SilentlyContinue | Out-Null
try { $lk.WaitForExit(5000) | Out-Null } catch {}
Check ($r.Exit -eq 0) "C: safe-listed owner cleared -> exit 0"
Check ($r.Out -match 'safe-kill') "C: owner was classified safe-kill"
Check ($r.Out -match 'probe deployed and hash verified') "C: deploy proceeded after clearance"
Check ((SandDllSha) -eq $SAFE) "C/H: final DLL is the pre-run baseline"

# ---- F: restore meets a temporary lock -> bounded retry wins ---------------
Reset-Sandbox
$sandDll = Join-Path $sand 'opengl32.dll'
$job = Start-Job -ArgumentList $sandDll, $INSTR -ScriptBlock {
    param($p, $want)
    # Wait until the harness has deployed the instrumented DLL, then hold it
    # for 5 s -- inside the harness's restore-retry budget.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 60) {
        # -EA Stop inside try: Get-FileHash's default non-terminating error
        # would land on the job's ERROR stream and detonate the strict-mode
        # driver at Receive-Job (it collides with the harness's own brief
        # exclusive-open windows -- Test-Replaceable, staged Replace).
        try { if ((Get-FileHash $p -ErrorAction Stop).Hash -eq $want) { break } } catch {}
        Start-Sleep -Milliseconds 200
    }
    try {
        $fs = [IO.File]::Open($p, 'Open', 'Read', 'Read')
        Start-Sleep -Seconds 5
        $fs.Close()
        return 'held'
    } catch { return "locker failed: $($_.Exception.Message)" }
}
$r = Run-Harness @('-PostDeployHoldSec','6','-RestoreRetrySec','30')
$jr = Receive-Job -Job $job -Wait; Remove-Job $job -Force
Check ($jr -eq 'held') "F: test locker actually held the deployed DLL"
Check ($r.Exit -eq 0) "F: restore succeeded within the retry budget -> exit 0"
Check ($r.Out -match 'RESTORED and verified') "F: restore verified after the wait"
Check ((SandDllSha) -eq $SAFE) "F/H: final DLL is the pre-run baseline"

# ---- G: restore lock outlives the budget -> loud fatal, exit 3, backup kept
Reset-Sandbox
$rel = Join-Path $sand 'release-G'
$job = Start-Job -ArgumentList $sandDll, $INSTR, $rel -ScriptBlock {
    param($p, $want, $release)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 60) {
        # -EA Stop inside try: Get-FileHash's default non-terminating error
        # would land on the job's ERROR stream and detonate the strict-mode
        # driver at Receive-Job (it collides with the harness's own brief
        # exclusive-open windows -- Test-Replaceable, staged Replace).
        try { if ((Get-FileHash $p -ErrorAction Stop).Hash -eq $want) { break } } catch {}
        Start-Sleep -Milliseconds 200
    }
    try {
        $fs = [IO.File]::Open($p, 'Open', 'Read', 'Read')
        $sw2 = [Diagnostics.Stopwatch]::StartNew()
        while ($sw2.Elapsed.TotalSeconds -lt 90 -and -not (Test-Path $release)) { Start-Sleep -Milliseconds 200 }
        $fs.Close()
        return 'held'
    } catch { return "locker failed: $($_.Exception.Message)" }
}
$r = Run-Harness @('-PostDeployHoldSec','6','-RestoreRetrySec','6')
New-Item $rel -ItemType File -Force | Out-Null
$jr = Receive-Job -Job $job -Wait; Remove-Job $job -Force
Check ($jr -eq 'held') "G: test locker held the deployed DLL past the budget"
Check ($r.Exit -eq 3) "G: unrecoverable restore -> exit 3, success NOT claimed"
Check ($r.Out -match 'RESTORATION THREW|RESTORATION FAILED') "G: loud fatal report printed"
Check ($r.Out -match 'prerun') "G: report points at the exact pre-run backup"
$g_runDir = Get-ChildItem (Join-Path $root 'runs') -Directory -Filter 'vbogame-locktest-*' |
            Sort-Object Name | Select-Object -Last 1
Check ((Test-Path (Join-Path $g_runDir.FullName 'prerun\opengl32.dll')) -and
       ((Get-FileHash (Join-Path $g_runDir.FullName 'prerun\opengl32.dll')).Hash -eq $SAFE)) "G: pre-run backup exists and is the baseline"
Check ((SandDllSha) -ne $SAFE) "H: exit 3 correctly reflects that the wrong DLL is still deployed"

# ------------------------------------------------------------------ summary
Write-Host ""
Write-Host "=== lock self-test: $pass passed, $fail failed ===" -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
if (-not $KeepSandbox) {
    Remove-Item $sand -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $root 'runs') -Directory -Filter 'vbogame-locktest-*' -EA SilentlyContinue |
        Remove-Item -Recurse -Force -EA SilentlyContinue
}
exit $(if ($fail) { 1 } else { 0 })
