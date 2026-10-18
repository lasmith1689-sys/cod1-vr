<#
  Self-test for the vbogame identity + milestone protocol
  (tools/vbogame_lib.ps1 -- the exact code vbogame_run.ps1 dot-sources).

  Covers the requirement list A-F (external module identity, no reliance on
  internal logging), G (stale log), H-equivalents (reader robustness), plus
  the two regressions that killed healthy runs on 2026-08-27:
    13:42  ReadAllText blinded by the writer's handle          -> reader fix
    15:53  fopen_s denies ALL sharing, so NO reader could open -> (a) writer
           now _SH_DENYWR, proven live against the real DLL via
           selftest_host; (b) identity no longer depends on the log at all.
  Restore-on-every-failure (requirement H) is covered end-to-end by
  tools/test_vbogame_locks.ps1.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vbogame_lib.ps1')
$root = Split-Path $PSScriptRoot -Parent

$pass = 0; $fail = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  PASS  $name" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "  FAIL  $name" -ForegroundColor Red;   $script:fail++ }
}

$GOOD = 'AAAABBBBCCCCDDDDEEEEFFFF0000111122223333444455556666777788889999'
$BAD  = 'B3705FE51711C0DCA623159C6317A79CD4640BB7B4583E3B6345165182249CC6'

function LogWith([string[]]$blocks) {
    $t = "=== COD VR camera probe ===`r`nhost: X pid 1`r`n"
    foreach ($b in $blocks) {
        switch ($b) {
            'id'     { $t += "=== CODVR BUILD ID ===`r`nbuild id/hash: $GOOD`r`nmodule path: X`r`n======================`r`n" }
            'badid'  { $t += "=== CODVR BUILD ID ===`r`nbuild id/hash: $BAD`r`nmodule path: X`r`n======================`r`n" }
            'proxy'  { $t += "=== PROXY FIRST CALL === (wglCreateContext)`r`n" }
            'cfg'    { $t += "=== VBOGAME PROBE CONFIG ===`r`narmed: YES`r`nhandsolve: YES`r`nVBO shadow: YES`r`n============================`r`n" }
            'cgame'  { $t += "cgame @ 3A6A0000 hooked (slot X)`r`n" }
            'scene'  { $t += "=== FIRST 3D SCENE ===`r`n" }
            'vm'     { $t += "=== FIRST VIEWMODEL DRAW ===`r`n" }
            'vbo'    { $t += "=== VBO SHADOW RESOLVED ===`r`n" }
            'palmR'  { $t += "=== RIGHT PALM SOLVED ===`r`n" }
            'palmL'  { $t += "=== LEFT PALM SOLVED ===`r`n" }
            'palmOK' { $t += "=== REAL PALM PATH ACTIVE ===`r`n" }
        }
    }
    return $t
}

Write-Host "=== vbogame identity + milestone self-test ===" -ForegroundColor Cyan

# ---- A: a real process that loads the instrumented proxy is identified by
# EXTERNAL module enumeration + on-disk hash, with no use of its log --------
$distDll  = Join-Path $root 'dist\opengl32.dll'
$distSha  = (Get-FileHash $distDll).Hash
$sthOut   = Join-Path $env:TEMP 'sth_ms_out.txt'
$identified = $false; $livePaths = @(); $liveRead = $false; $attempts = 0
while (-not $identified -and $attempts -lt 3) {
    $attempts++
    Remove-Item (Join-Path $root 'dist\codvr_result.log') -EA SilentlyContinue
    $p = Start-Process (Join-Path $root 'build\selftest_host.exe') `
         -ArgumentList "`"$distDll`"" -PassThru -WindowStyle Hidden `
         -RedirectStandardOutput $sthOut
    while (-not $p.HasExited) {
        $livePaths = Get-ProcessOpenGLModules $p
        if ($livePaths.Count -gt 0 -and -not $identified) {
            $id = Resolve-OpenGLIdentity $livePaths $distDll $distSha
            if ($id.Status -eq 'PASS') { $identified = $true }
        }
        # the writer-share regression: the REAL DLL's logger must now allow
        # a live external read (it was fopen_s = deny-all until 2026-08-27)
        $t = Read-LiveLogText (Join-Path $root 'dist\codvr_result.log')
        if ($t.Length -gt 0) { $liveRead = $true }
        Start-Sleep -Milliseconds 25
    }
}
Check $identified "A: live process's app-local proxy identified externally (module path + on-disk hash)"
Check ($livePaths -contains $distDll) "A: the enumerated module path IS the staged DLL's path"
Check $liveRead "A/F2: the DLL's OWN log is now readable while the process lives (_SH_DENYWR fix, proven live)"

# ---- B: correct app-local path + hash => PASS even with NO log content ----
$tmpDir = Join-Path $env:TEMP 'codvr_ms_mods'; New-Item -ItemType Directory -Force $tmpDir | Out-Null
$fakeApp = Join-Path $tmpDir 'opengl32.dll'
Set-Content $fakeApp 'fake-proxy-bytes' -Encoding ascii
$fakeSha = (Get-FileHash $fakeApp).Hash
$id = Resolve-OpenGLIdentity @($fakeApp, 'C:\WINDOWS\system32\opengl32.dll') $fakeApp $fakeSha
Check ($id.Status -eq 'PASS') "B: app-local path + matching hash -> PASS (no log involved)"
$ms = Get-VbogameMilestones '' $GOOD $true $true
Check ($ms.Stages['APPLOCAL_OPENGL32_LOADED'] -and $ms.Deepest -eq 'APPLOCAL_OPENGL32_LOADED') "B: identity stage stands with an EMPTY/unreadable log"

# ---- C: wrong/system-only opengl32 => clear identity FAIL ------------------
$id = Resolve-OpenGLIdentity @('C:\WINDOWS\system32\opengl32.dll') $fakeApp $fakeSha
Check ($id.Status -eq 'WRONG_PATH') "C: only the system DLL loaded -> WRONG_PATH"
Set-Content $fakeApp 'tampered-bytes' -Encoding ascii
$id = Resolve-OpenGLIdentity @($fakeApp) $fakeApp $fakeSha
Check ($id.Status -eq 'HASH_MISMATCH') "C2: right path, wrong bytes -> HASH_MISMATCH"
$id = Resolve-OpenGLIdentity @() $fakeApp $fakeSha
Check ($id.Status -eq 'NOT_LOADED') "C3: no opengl32 module yet -> NOT_LOADED (wait, don't fail)"
Remove-Item $tmpDir -Recurse -Force

# ---- D: the internal build id arrives later as a CROSS-CHECK ---------------
$ms = Get-VbogameMilestones (LogWith @('id','proxy','cfg')) $GOOD $true $true
Check ($ms.CrossCheck -eq 'MATCH' -and $ms.Verdict -eq '') "D: matching build id cross-checks the external identity"
$ms = Get-VbogameMilestones (LogWith @('badid','proxy','cfg')) $GOOD $true $true
Check ($ms.Verdict -like 'WRONG_BINARY*') "D2: a PRESENT-and-mismatched build id is the one thing that means wrong binary"
$ms = Get-VbogameMilestones (LogWith @('proxy','cfg')) $GOOD $true $true
Check ($ms.CrossCheck -eq 'ABSENT' -and $ms.Verdict -eq '') "D3: an absent build id is ABSENT, never a failure"

# ---- E: no reliance on DllMain logging -- milestones flow from normal
# proxy execution and identity from the module list --------------------------
$ms = Get-VbogameMilestones (LogWith @('proxy','cfg','cgame','scene')) $GOOD $true $true
Check ($ms.Stages['PROXY_FIRST_CALL'] -and $ms.Stages['PROBE_CONFIG_ARMED'] -and
       $ms.Deepest -eq 'FIRST_3D_SCENE') "E: full pre-click ladder passes with NO build-id block at all"

# ---- F: a log whose writer denies ALL sharing cannot blind identity --------
$tmp = Join-Path $env:TEMP 'codvr_ms_denyall.log'
$w = New-Object IO.FileStream($tmp, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
$live = Read-LiveLogText $tmp
$ms = Get-VbogameMilestones $live $GOOD $true $true
$w.Close(); Remove-Item $tmp -Force
Check ($live -eq '') "F: a deny-all writer makes the log unreadable (the 15:53 mechanism, reproduced)"
Check ($ms.Stages['APPLOCAL_OPENGL32_LOADED'] -and $ms.Verdict -eq '') "F: ...and identity STILL stands, with no adverse verdict"

# ---- G: a stale log from another build cannot satisfy the current run ------
$ms = Get-VbogameMilestones (LogWith @('badid','proxy','cfg','scene','vm','vbo','palmR','palmL','palmOK')) $GOOD $true $true
Check ($ms.Verdict -like 'WRONG_BINARY*') "G: a stale full-success log from another build is rejected by hash"

# ---- H: replacement / truncation handled -----------------------------------
$tmp = Join-Path $env:TEMP 'codvr_ms_testH.log'
Set-Content $tmp (LogWith @('id','proxy','cfg','scene')) -Encoding ascii
$r1 = Get-VbogameMilestones (Read-LiveLogText $tmp) $GOOD $true $true
Set-Content $tmp (LogWith @('id')) -Encoding ascii
$r2 = Get-VbogameMilestones (Read-LiveLogText $tmp) $GOOD $true $true
Remove-Item $tmp -Force
Check ($r1.Stages['FIRST_3D_SCENE'] -and -not $r2.Stages['FIRST_3D_SCENE']) "H: a replaced/truncated log re-parses cleanly from byte 0"

# ---- the full ladder -------------------------------------------------------
$ms = Get-VbogameMilestones (LogWith @('id','proxy','cfg','cgame','scene','vm','vbo','palmR','palmL','palmOK')) $GOOD $true $true
Check ($ms.Deepest -eq 'REAL_PALM_PATH_ACTIVE') "full run: deepest is REAL_PALM_PATH_ACTIVE"

Write-Host ""
Write-Host "=== identity/milestone self-test: $pass passed, $fail failed ===" -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
exit $(if ($fail) { 1 } else { 0 })
