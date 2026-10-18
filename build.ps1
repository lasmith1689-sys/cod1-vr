<#
  Builds the 32-bit opengl32.dll proxy that carries the VR mod.

  Everything is x86 because CoD1 is a 32-bit process -- including the OpenXR
  loader, which is why the Win32 (not x64) loader from the SDK is linked.

  THE GATE: -SelfTest runs the DLL's own suite (through selftest_host) AND the
  safe renderer suite (vr_safetest: vr.cpp/comfort.cpp against a mocked OpenXR
  runtime plus a real hidden GL context). Any FAIL line, a missing result log,
  a nonzero test exit, a wrong export count, or a non-x86 binary FAILS the
  build. -Deploy always runs the gate first: a red build cannot deploy.

  -GateSelfCheck deliberately feeds the log parser a failing log and expects it
  to throw, proving the gate itself is alive.
#>
param([switch]$Deploy, [switch]$SelfTest, [switch]$GateSelfCheck,
      [string]$VcVars, [string]$GameDir, [string]$Python)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$src     = Join-Path $root 'src'
$out     = Join-Path $root 'build'
$dist    = Join-Path $root 'dist'
$testsrc = Join-Path $root 'tests'
$xrInc   = Join-Path $root 'third_party\openxr\include'
$xrLib   = Join-Path $root 'third_party\openxr\native\Win32\release\lib\openxr_loader.lib'
$xrDll   = Join-Path $root 'third_party\openxr\native\Win32\release\bin\openxr_loader.dll'
# ------------------------------------------------------------------ locations
#
# The toolchain, Python and the game folder differ per machine, so none of them
# is baked in. Override any of them with a parameter (-VcVars / -Python /
# -GameDir) or an environment variable (CODVR_VCVARS / CODVR_PYTHON /
# CODVR_GAME_DIR); left unset, each is discovered below.

function Find-VcVars {
    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86) {
        $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            foreach ($install in (& $vswhere -latest -prerelease -products * -property installationPath 2>$null)) {
                $bat = Join-Path $install 'VC\Auxiliary\Build\vcvarsall.bat'
                if (Test-Path $bat) { return $bat }
            }
        }
    }
    # vswhere is absent from some Build Tools-only installs: glob instead, and
    # take the newest layout.
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $root) { continue }
        $bat = Get-ChildItem -Path (Join-Path $root 'Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvarsall.bat') -ErrorAction SilentlyContinue |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($bat) { return $bat.FullName }
    }
    return $null
}

function Find-Python {
    foreach ($name in @('python.exe', 'py.exe')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    $glob = Get-ChildItem -Path 'C:\Python3*\python.exe' -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
    if ($glob) { return $glob.FullName }
    return $null
}

function Find-GameDir {
    $libraries = @()

    # Steam's own install, plus every library folder it has registered.
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
                       'HKLM:\SOFTWARE\Valve\Steam')) {
        $steam = (Get-ItemProperty -Path $key -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
        if (-not $steam) { continue }
        $libraries += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }

    # No registry key, or a library Steam has forgotten: the usual folder names
    # on every filesystem drive.
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        foreach ($name in @('SteamLibrary', 'Steam', 'Games\Steam', 'Program Files (x86)\Steam')) {
            $libraries += (Join-Path $drive.Root $name)
        }
    }

    # CoDSP.exe is the single-player executable the mod sits beside.
    foreach ($lib in $libraries) {
        $candidate = Join-Path $lib 'steamapps\common\Call of Duty'
        if (Test-Path (Join-Path $candidate 'CoDSP.exe')) { return $candidate }
    }
    return $null
}

$vcvars  = if ($VcVars)  { $VcVars }  elseif ($env:CODVR_VCVARS)   { $env:CODVR_VCVARS }   else { Find-VcVars }
$python  = if ($Python)  { $Python }  elseif ($env:CODVR_PYTHON)   { $env:CODVR_PYTHON }   else { Find-Python }
$gameDir = if ($GameDir) { $GameDir } elseif ($env:CODVR_GAME_DIR) { $env:CODVR_GAME_DIR } else { Find-GameDir }

# vcvarsall.bat locates its own helpers (vswhere.exe among them) through PATH,
# so an unusual PATH -- a Git Bash / MSYS shell, a trimmed CI environment --
# makes it fail with "'vswhere.exe' is not recognized" and then "\Microsoft was
# unexpected at this time", exiting 255 with no compiler output to explain it.
# Every generated batch therefore starts from a known-good PATH and checks that
# the toolchain really came up, so a build never depends on the calling shell.
$batHeader = @(
    '@echo off'
    'set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"'
    "call `"$vcvars`" x86 >nul 2>&1"
    'if errorlevel 1 (echo FAILED: vcvarsall.bat could not set up the x86 toolchain & exit /b 1)'
)

# Deploying without the gate is how a red build reaches the game folder.
if ($Deploy) { $SelfTest = $true }

# ---------------------------------------------------------------- gate logic
#
# One function owns the pass/fail decision for a self-test result log, so the
# rule is in exactly one place and -GateSelfCheck can prove it fires.
function Assert-SelfTestLog {
    param([string]$LogPath, [string]$Label)

    if (-not (Test-Path $LogPath)) {
        throw "GATE [$Label]: result log missing at $LogPath"
    }
    $lines = Get-Content $LogPath
    $failLines = @($lines | Where-Object { $_ -match '^\s*FAIL\s\s' })
    if ($failLines.Count -gt 0) {
        $failLines | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        throw "GATE [$Label]: $($failLines.Count) failing test(s)"
    }
    $summary = @($lines | Where-Object { $_ -match '=== SELF TEST: (\d+) passed, (\d+) failed ===' })
    if ($summary.Count -eq 0) {
        throw "GATE [$Label]: no summary line -- suite did not finish"
    }
    $null = $summary[-1] -match '=== SELF TEST: (\d+) passed, (\d+) failed ==='
    $passed = [int]$Matches[1]; $failed = [int]$Matches[2]
    if ($failed -ne 0) { throw "GATE [$Label]: summary reports $failed failed" }
    if ($passed -le 0) { throw "GATE [$Label]: summary reports zero tests" }
    Write-Host "  gate [$Label]: $passed passed, 0 failed" -ForegroundColor Green
    return $passed
}

if ($GateSelfCheck) {
    Write-Host 'gate self-check: feeding the parser a deliberately red log' -ForegroundColor Cyan
    $tmp = Join-Path $env:TEMP 'codvr_gatecheck.log'
    @('  PASS  something fine', '  FAIL  something broken',
      '=== SELF TEST: 1 passed, 1 failed ===') | Set-Content $tmp -Encoding ascii
    $threw = $false
    try { Assert-SelfTestLog -LogPath $tmp -Label 'self-check red' | Out-Null }
    catch { $threw = $true; Write-Host "  caught as expected: $_" -ForegroundColor Green }
    if (-not $threw) { throw 'GATE SELF-CHECK FAILED: a red log was accepted' }
    $missingThrew = $false
    try { Assert-SelfTestLog -LogPath (Join-Path $env:TEMP 'codvr_no_such.log') -Label 'self-check missing' | Out-Null }
    catch { $missingThrew = $true }
    if (-not $missingThrew) { throw 'GATE SELF-CHECK FAILED: a missing log was accepted' }
    Remove-Item $tmp -ErrorAction SilentlyContinue
    Write-Host 'gate self-check ok' -ForegroundColor Green
    return
}

if (-not $vcvars) {
    throw ("missing prerequisite: vcvarsall.bat (the MSVC x86 toolchain). Install " +
           "'Desktop development with C++' from Visual Studio or Build Tools, or " +
           "point the build at it with -VcVars <path> / CODVR_VCVARS.")
}
if (-not $python) {
    throw ("missing prerequisite: python (it regenerates the export thunks). " +
           "Install Python 3, or point the build at it with -Python <path> / CODVR_PYTHON.")
}
foreach ($p in @($vcvars, $xrLib, $xrDll)) {
    if (-not (Test-Path $p)) { throw "missing prerequisite: $p" }
}
New-Item -ItemType Directory -Force -Path $out, $dist | Out-Null

Write-Host '[1/5] regenerating export thunks' -ForegroundColor Cyan
& $python (Join-Path $root 'tools\gen_gl_proxy.py')
if ($LASTEXITCODE -ne 0) { throw 'export generation failed' }

Write-Host '[2/5] compiling (x86)' -ForegroundColor Cyan
$bat = Join-Path $out '_build.bat'
$lines = $batHeader + @(
    ('cl /nologo /c /O2 /MT /W3 /EHsc /GS- /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS ' +
     "/I`"$src`" /I`"$xrInc`" /Fo`"$out\\`" " +
     "`"$src\dllmain.cpp`" `"$src\vr.cpp`" `"$src\probe.cpp`" `"$src\comfort.cpp`" " +
     "`"$src\vr_input.cpp`" `"$src\vr_weapon.cpp`" `"$src\vr_binds.cpp`" " +
     "`"$src\vr_glsurf.cpp`" `"$src\vr_vbo.cpp`" `"$src\vr_objects.cpp`" `"$src\vr_cfg_contract.cpp`" `"$src\vr_handpose.cpp`" `"$src\vr_weapon_profile.cpp`" `"$src\vr_reload.cpp`" `"$src\vr_native_hands.cpp`" `"$src\vr_menu.cpp`" `"$src\vr_snapshot.cpp`"")
    'if errorlevel 1 exit /b 1'
    # Delay-load the OpenXR loader. Linked normally it becomes a hard import,
    # and a missing openxr_loader.dll would stop opengl32.dll loading at all --
    # which means the game would refuse to start rather than just run flat.
    # /Brepro replaces both PE TimeDateStamp fields (COFF header and debug
    # directory) with a hash of the content. Without it the same sources
    # produce a DLL that differs from the last build in exactly four bytes,
    # so "does the deployed binary come from this tree" can only be answered
    # by diffing around the timestamps instead of by comparing one hash.
    ("link /nologo /DLL /MACHINE:X86 /Brepro /DEF:`"$src\opengl32.def`" " +
     "/OUT:`"$out\opengl32.dll`" /IMPLIB:`"$out\opengl32.lib`" " +
     '/DELAYLOAD:openxr_loader.dll ' +
     "`"$out\dllmain.obj`" `"$out\vr.obj`" `"$out\probe.obj`" `"$out\comfort.obj`" " +
     "`"$out\vr_input.obj`" `"$out\vr_weapon.obj`" `"$out\vr_binds.obj`" " +
     "`"$out\vr_glsurf.obj`" `"$out\vr_vbo.obj`" `"$out\vr_objects.obj`" `"$out\vr_cfg_contract.obj`" `"$out\vr_handpose.obj`" `"$out\vr_weapon_profile.obj`" `"$out\vr_reload.obj`" `"$out\vr_native_hands.obj`" `"$out\vr_menu.obj`" `"$out\vr_snapshot.obj`" `"$xrLib`" " +
     'delayimp.lib kernel32.lib user32.lib gdi32.lib advapi32.lib')
    'if errorlevel 1 exit /b 1'
    'exit /b 0'
)
Set-Content -Path $bat -Value $lines -Encoding ascii

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& cmd /c "`"$bat`"" 2>&1 | ForEach-Object { Write-Host $_ }
$code = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($code -ne 0) { throw "build failed (exit $code)" }

Copy-Item "$out\opengl32.dll" $dist -Force
Copy-Item $xrDll $dist -Force
Write-Host "[3/5] built $dist\opengl32.dll" -ForegroundColor Green

# Verify we produced a 32-bit DLL whose export table still matches the real
# one. This is part of the gate: exit 1 on any mismatch, and the build throws.
$verify = @"
import struct, sys
d=open(r'$out\opengl32.dll','rb').read()
pe=struct.unpack_from('<I',d,0x3C)[0]
mach=struct.unpack_from('<H',d,pe+4)[0]
ok = True
if mach != 0x14c:
    print('  machine : NOT x86 (0x%X) -- GATE FAIL' % mach); ok = False
else:
    print('  machine : x86 (32-bit) OK')
opt=pe+24; n=struct.unpack_from('<H',d,pe+6)[0]; osz=struct.unpack_from('<H',d,pe+20)[0]
erva,_=struct.unpack_from('<II',d,opt+96)
secs=[]
for i in range(n):
    s=opt+osz+i*40
    vs,va,rs,ro=struct.unpack_from('<IIII',d,s+8); secs.append((va,vs,ro,rs))
def o(r):
    for va,vs,ro,rs in secs:
        if va<=r<va+max(vs,rs): return ro+(r-va)
    raise SystemExit('bad rva')
nexp = struct.unpack_from('<I',d,o(erva)+24)[0]
print('  exports :', nexp, '(must be 368)')
if nexp != 368: ok = False
sys.exit(0 if ok else 1)
"@
$verify | & $python -
if ($LASTEXITCODE -ne 0) { throw 'GATE: export/machine verification failed' }

if ($SelfTest) {
    Write-Host '[4/5] test gate' -ForegroundColor Cyan

    # ---- 4a: the safe renderer suite (mocked OpenXR + real hidden GL) ----
    $safeMain = Join-Path $testsrc 'vr_safetest.cpp'
    if (Test-Path $safeMain) {
        $safeBat = Join-Path $out '_buildsafe.bat'
        $safeLines = $batHeader + @(
            ("cl /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS " +
             "/I`"$src`" /I`"$xrInc`" /Fo`"$out\\`" /Fe`"$out\vr_safetest.exe`" " +
             "`"$src\vr.cpp`" `"$src\comfort.cpp`" `"$src\vr_input.cpp`" " +
             "`"$src\vr_weapon.cpp`" `"$src\vr_binds.cpp`" `"$src\vr_glsurf.cpp`" " +
             "`"$src\vr_vbo.cpp`" `"$src\vr_objects.cpp`" `"$src\vr_cfg_contract.cpp`" `"$src\vr_handpose.cpp`" `"$src\vr_weapon_profile.cpp`" `"$src\vr_reload.cpp`" `"$src\vr_native_hands.cpp`" `"$src\vr_menu.cpp`" `"$src\vr_snapshot.cpp`" " +
             "`"$testsrc\xr_mock.cpp`" `"$testsrc\vr_safetest.cpp`" " +
             'user32.lib gdi32.lib')
            'if errorlevel 1 exit /b 1'
            'exit /b 0'
        )
        Set-Content -Path $safeBat -Value $safeLines -Encoding ascii
        $prevS = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & cmd /c "`"$safeBat`"" 2>&1 | ForEach-Object { Write-Host "  $_" }
        $safeBuild = $LASTEXITCODE
        $ErrorActionPreference = $prevS
        if ($safeBuild -ne 0) { throw 'GATE: safe test suite failed to build' }

        # VR_Startup probes for openxr_loader.dll with LoadLibrary even though
        # the mock provides every xr* symbol; the file just has to exist.
        Copy-Item $xrDll (Join-Path $out 'openxr_loader.dll') -Force

        $prevS = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & "$out\vr_safetest.exe" 2>&1 | ForEach-Object { Write-Host "  $_" }
        $safeRun = $LASTEXITCODE
        $ErrorActionPreference = $prevS
        if ($safeRun -ne 0) { throw "GATE: safe test suite FAILED (exit $safeRun)" }
        Write-Host '  gate [safe suite]: green' -ForegroundColor Green
    } else {
        throw "GATE: safe test suite missing at $safeMain"
    }

    # ---- 4b: the DLL's own suite via the host --------------------------
    # Rebuild the host too: it brings up a real GL context so the DLL's tests
    # can exercise the capture/blit paths against a live driver.
    $hostBat = Join-Path $out '_buildhost.bat'
    $hostLines = $batHeader + @(
        ("cl /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS " +
         "/Fo`"$out\\`" /Fe`"$out\selftest_host.exe`" " +
         "`"$root\tools\selftest_host.cpp`" user32.lib gdi32.lib")
        'if errorlevel 1 exit /b 1'
        'exit /b 0'
    )
    Set-Content -Path $hostBat -Value $hostLines -Encoding ascii
    $prevH = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & cmd /c "`"$hostBat`"" 2>&1 | ForEach-Object { Write-Host "  $_" }
    $hostCode = $LASTEXITCODE
    $ErrorActionPreference = $prevH
    if ($hostCode -ne 0) { throw 'GATE: self-test host failed to build' }

    Remove-Item "$dist\codvr_result.log" -ErrorAction SilentlyContinue

    # The DLL suite checks its derived syscall slot against the real
    # cgamex86.dll when it can find the game. Tell it where; unset, that one
    # check logs a skip rather than failing (see dllmain.cpp).
    if ($gameDir) { $env:CODVR_GAME_DIR = $gameDir }
    $prevH = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & "$out\selftest_host.exe" "$dist\opengl32.dll" 2>&1 | ForEach-Object { Write-Host "  $_" }
    $hostRun = $LASTEXITCODE
    $ErrorActionPreference = $prevH
    if ($hostRun -ne 0) { throw "GATE: self-test host reported failure (exit $hostRun)" }

    # Belt and braces: the host already parsed the log; parse it here too so
    # the build's verdict never depends on a single implementation.
    $dllTests = Assert-SelfTestLog -LogPath "$dist\codvr_result.log" -Label 'dll suite'
    if (Test-Path "$dist\codvr_result.log") {
        Get-Content "$dist\codvr_result.log" | Where-Object { $_ -match 'SELF TEST|VR:' } |
            Select-Object -Last 4 | ForEach-Object { Write-Host "  $_" }
    }
} else {
    Write-Host '[4/5] test gate skipped (pass -SelfTest; -Deploy always runs it)' -ForegroundColor DarkGray
}

if ($Deploy) {
    if (-not $gameDir) {
        throw ("game folder not found: pass -GameDir <path> or set CODVR_GAME_DIR " +
               "to the folder that holds CoDSP.exe.")
    }
    if (-not (Test-Path $gameDir)) { throw "game dir not found: $gameDir" }

    # ---- deploy-time contract check ------------------------------------
    #
    # The config about to be copied into the game folder is READ, not assumed.
    # A probe-armed cfg reaching a player is the 2026-08-25 incident; the
    # build refuses to be the thing that carries one there.
    $cfgSrc  = Join-Path $dist 'codvr.cfg'
    if (-not (Test-Path $cfgSrc)) { throw "GATE: no codvr.cfg in $dist" }
    $cfgText = Get-Content $cfgSrc
    $armLine = @($cfgText | Where-Object { $_ -match '^\s*vr_probe_arm\s+[1-9]' })
    if ($armLine.Count -gt 0) {
        throw "GATE: $cfgSrc arms the probe system (vr_probe_arm) -- refusing to deploy a capture rig"
    }
    # Belt and braces: the DLL ignores these unarmed, but a shipped config
    # should not carry them at all.
    foreach ($k in 'vr_disable','vr_glprobe_sweep','vr_glcap_autoquit','vr_glprobe_class') {
        $hit = @($cfgText | Where-Object { $_ -match "^\s*$k\s+[1-9]" })
        if ($hit.Count -gt 0) {
            Write-Host "  WARNING: shipped cfg carries '$k' (inert without vr_probe_arm, but stale)" -ForegroundColor Yellow
        }
    }
    Write-Host '  deploy contract: codvr.cfg is unarmed (VR on, sweep off, autoquit off)' -ForegroundColor Green

    Copy-Item "$dist\opengl32.dll" (Join-Path $gameDir 'opengl32.dll') -Force
    # The OpenXR loader must sit beside the game exe so the DLL can find it.
    Copy-Item $xrDll (Join-Path $gameDir 'openxr_loader.dll') -Force
    # The config lives beside the DLL in the game folder; deploying only the
    # DLL let the two drift apart silently.
    Copy-Item $cfgSrc (Join-Path $gameDir 'codvr.cfg') -Force
    # Retire the older data pack that zeroed spread and view kick globally.
    # This build narrows only the active player's adsSpread during two-hand
    # ADS and deliberately leaves recoil/view kick untouched.  Rename rather
    # than delete so the prior behaviour remains recoverable.
    $oldPrecisionPk3 = Join-Path $gameDir 'main\zz_codvr_precision.pk3'
    if (Test-Path $oldPrecisionPk3) {
        $retiredPrecisionPk3 = "$oldPrecisionPk3.disabled-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Move-Item -LiteralPath $oldPrecisionPk3 -Destination $retiredPrecisionPk3
        Write-Host "  retired global no-spread pack -> $retiredPrecisionPk3" -ForegroundColor Yellow
    }
    $autoSrc = Join-Path $dist 'autoexec.cfg'
    if (Test-Path $autoSrc) {
        Copy-Item $autoSrc (Join-Path $gameDir 'main\autoexec.cfg') -Force
        Write-Host '  deployed main\autoexec.cfg' -ForegroundColor Yellow

        # Renderer-latched cvars are also written to config.cfg so they are in
        # force before renderer initialization on this first launch. autoexec
        # reapplies the same values on every later launch if the game rewrites
        # config.cfg while shutting down. Do not touch resolution, render
        # scale, FOV, gamma, pacing, upscaling, or any unrelated cvar here.
        $quality = [ordered]@{
            r_picmip = '0'; r_picmip2 = '0'; r_texturebits = '32'; r_colorbits = '32'
            r_texturemode = 'GL_LINEAR_MIPMAP_LINEAR'
            r_simpleMipMaps = '0'; r_detailtextures = '1'; r_drawSun = '1'
            r_dynamiclight = '1'; r_dlightQuality = '1'; r_diffuseSunQuality = '2'
            r_diffuseSunSteps = '3'; r_maxEntLights = '8'; r_fastsky = '0'; r_flares = '1'
            cg_marks = '1'; cg_brass = '1'; ai_corpseCount = '64'
            r_lodbias = '0'; r_lodscale = '0'
        }
        $gameConfig = Join-Path $gameDir 'main\config.cfg'
        if (Test-Path $gameConfig) {
            $configLines = [Collections.Generic.List[string]]::new()
            Get-Content -LiteralPath $gameConfig | ForEach-Object { $configLines.Add($_) }
            foreach ($entry in $quality.GetEnumerator()) {
                $pattern = '^\s*(?:seta\s+)?' + [regex]::Escape($entry.Key) + '\s+'
                $replacement = 'seta ' + $entry.Key + ' "' + $entry.Value + '"'
                $found = $false
                for ($i = 0; $i -lt $configLines.Count; $i++) {
                    if ($configLines[$i] -match $pattern) {
                        if (-not $found) { $configLines[$i] = $replacement; $found = $true }
                        else { $configLines.RemoveAt($i); $i-- }
                    }
                }
                if (-not $found) { $configLines.Add($replacement) }
            }
            [IO.File]::WriteAllLines($gameConfig, $configLines, [Text.Encoding]::ASCII)
            Write-Host "  applied $($quality.Count) supported maximum-detail cvars to main\config.cfg" -ForegroundColor Yellow
        }
    }
    Write-Host "[5/5] deployed opengl32.dll + openxr_loader.dll + codvr.cfg -> $gameDir" -ForegroundColor Yellow

    # ---- provenance ------------------------------------------------------
    #
    # "Is the DLL in the game folder the one this tree just built?" must be
    # answerable by comparing two numbers, not by trusting that the copy
    # worked. /Brepro makes the build reproducible, so a matching hash really
    # does mean matching sources.
    $builtDll = Get-FileHash "$out\opengl32.dll" -Algorithm SHA256
    $distDll  = Get-FileHash "$dist\opengl32.dll" -Algorithm SHA256
    $gameDll  = Get-FileHash (Join-Path $gameDir 'opengl32.dll') -Algorithm SHA256
    $gameCfg  = Get-FileHash (Join-Path $gameDir 'codvr.cfg')   -Algorithm SHA256
    $srcHash  = (Get-ChildItem $src -File | Sort-Object Name | ForEach-Object {
                    (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ''
    $srcRev   = (Get-FileHash -InputStream ([IO.MemoryStream]::new(
                    [Text.Encoding]::ASCII.GetBytes($srcHash))) -Algorithm SHA256).Hash

    Write-Host ''
    Write-Host '  ---- deploy provenance ----' -ForegroundColor Cyan
    Write-Host "  canonical source : $root"
    Write-Host "  source revision  : $srcRev"
    Write-Host "  built    DLL     : $($builtDll.Hash)"
    Write-Host "  dist     DLL     : $($distDll.Hash)"
    Write-Host "  deployed DLL     : $($gameDll.Hash)"
    Write-Host "  deployed cfg     : $($gameCfg.Hash)"
    if ($builtDll.Hash -ne $gameDll.Hash) {
        throw "GATE: deployed DLL does not match the build ($($builtDll.Hash) vs $($gameDll.Hash))"
    }
    Write-Host '  build == deployed: OK' -ForegroundColor Green

    $prov = [ordered]@{
        utc            = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        canonicalTree  = $root
        sourceRevision = $srcRev
        builtDll       = $builtDll.Hash
        deployedDll    = $gameDll.Hash
        deployedCfg    = $gameCfg.Hash
        deployedPk3    = $(if (Test-Path (Join-Path $gameDir 'main\zz_codvr_precision.pk3')) { (Get-FileHash (Join-Path $gameDir 'main\zz_codvr_precision.pk3') -Algorithm SHA256).Hash } else { '' })
        deployedAutoexec = $(if (Test-Path (Join-Path $gameDir 'main\autoexec.cfg')) { (Get-FileHash (Join-Path $gameDir 'main\autoexec.cfg') -Algorithm SHA256).Hash } else { '' })
        gameDir        = $gameDir
        probeArmed     = $false
    }
    $prov | ConvertTo-Json | Set-Content (Join-Path $dist 'deploy-provenance.json') -Encoding ascii
} else {
    Write-Host '[5/5] deploy skipped (pass -Deploy)' -ForegroundColor DarkGray
}



