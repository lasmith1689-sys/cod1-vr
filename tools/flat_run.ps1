<#
  One automated flat (monitor) run of CoD1 with the VR proxy.

  WHY THIS EXISTS. Every claim in this milestone about which draw call is the
  weapon, whether a transform moved it, and whether the world stayed put has to
  be evidence a script produced -- not something a person looked at once. The
  headset is the scarce resource; the monitor is not. So the run is scripted
  end to end: deploy, launch, let the mod write its own artefacts, close the
  game the way a user would, and collect.

  It never force-kills unless the timeout expires, because a forced
  termination is exactly what leaves the "Run In Safe Mode?" prompt behind for
  the NEXT run to trip over. The mod posts WM_CLOSE to itself when its work is
  done (see GLSurfQuitCleanly in dllmain.cpp), which is an ordinary engine
  shutdown.

  Usage:
    tools\flat_run.ps1 -Tag census -CfgLines @('vr_glcap_frames 2') `
                       -Expect codvr_draws_census.csv
#>
param(
    [string]   $Tag        = 'flat',
    [string[]] $CfgLines   = @(),
    [string]   $Map        = 'chateau',
    [int]      $TimeoutSec = 300,
    [string[]] $Expect     = @(),
    [switch]   $Screenshot,
    [switch]   $SkipBuild,
    [string[]] $ExtraArgs  = @(),
    [string[]] $AutoexecLines = @(),
    [int]      $Retries    = 2,
    [int]      $PrimeForSec = 60,
    # Deploy and wait; the person starts the game themselves. CoD1 SP holds a
    # mission-briefing menu open after `spmap` and pauses the client while any
    # menu is up (cl_paused 1), so nothing renders until a human clicks Start.
    # One click beats a harness that synthesises keystrokes into whatever
    # window Windows actually had in front.
    [switch]   $NoLaunch,
    # Deploy the build and the run's config, then return immediately. The
    # person plays when it suits them; -Collect gathers the artefacts later.
    # This is what keeps a hardware-in-the-loop step from having to happen
    # inside one command's timeout.
    [switch]   $DeployOnly,
    [switch]   $Collect
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$dist    = Join-Path $root 'dist'
$gameDir = 'H:\SteamLibrary\steamapps\common\Call of Duty'
$steam   = 'C:\Program Files (x86)\Steam\steam.exe'
$runDir  = Join-Path $root ("runs\" + $Tag)

foreach ($p in @($gameDir, $steam)) {
    if (-not (Test-Path $p)) { throw "missing prerequisite: $p" }
}

# ---------------------------------------------------------------- win32 bits
if (-not ("CodVrWin" -as [type])) {
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class CodVrWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int  GetWindowTextA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int  GetClassNameA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    public static void Tap(byte vk) {
        keybd_event(vk, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        keybd_event(vk, 0, 2, IntPtr.Zero);   // KEYEVENTF_KEYUP
    }
    public static IntPtr MainWindow(uint pid) {
        IntPtr best = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid || !IsWindowVisible(h)) return true;
            var cls = new StringBuilder(64); GetClassNameA(h, cls, 64);
            if (cls.ToString() != "#32770" && best == IntPtr.Zero) best = h;
            return true;
        }, IntPtr.Zero);
        return best;
    }
    public static bool Focus(uint pid) {
        IntPtr h = MainWindow(pid);
        if (h == IntPtr.Zero) return false;
        if (GetForegroundWindow() == h) return true;
        ShowWindow(h, 9);   // SW_RESTORE
        return SetForegroundWindow(h);
    }
    public static System.Collections.Generic.List<string> Dialogs(uint pid) {
        var found = new System.Collections.Generic.List<string>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid || !IsWindowVisible(h)) return true;
            var cls = new StringBuilder(64); GetClassNameA(h, cls, 64);
            var txt = new StringBuilder(256); GetWindowTextA(h, txt, 256);
            if (cls.ToString() == "#32770")
                found.Add(h.ToInt64().ToString() + "|" + txt.ToString());
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
}

function Restore-SafeDeployment {
    # The standing hygiene rule: a flat run ends with the game folder holding
    # the safe camera-stable deployment, hash-verified. Until 2026-08-27 this
    # existed only as a claim in docs/NATIVE-HANDS-PHASE-STATUS.md -- the DLL
    # restore was done by hand. That is how the 2026-08-27 attended run got
    # played on the safe baseline while everyone believed the VBO rig was
    # deployed: the capture DLL was (correctly) gone, but nothing scripted
    # says so out loud. Now the restore is code, and it verifies.
    $ckpt = Join-Path $root 'checkpoints\deployed-as-found-2026-08-24'
    $sums = Get-Content (Join-Path $ckpt 'SHA256SUMS.txt')
    $want = ($sums | Where-Object { $_ -match 'opengl32\.dll$' }) -split '\s+' | Select-Object -First 1
    if (-not $want -or $want.Length -ne 64) { throw "safe restore: cannot read opengl32.dll hash from $ckpt\SHA256SUMS.txt" }
    Copy-Item (Join-Path $ckpt 'opengl32.dll') (Join-Path $gameDir 'opengl32.dll') -Force
    $got = (Get-FileHash (Join-Path $gameDir 'opengl32.dll') -Algorithm SHA256).Hash
    if ($got -ne $want) { throw "safe restore FAILED: deployed opengl32.dll = $got, want $want" }
    Write-Host "  safe deployment restored (opengl32.dll $($want.Substring(0,8))... verified)" -ForegroundColor DarkGray
}

function Dismiss-GameDialogs([int]$pid_) {
    # A "Run In Safe Mode?" prompt (or any other modal) blocks the run
    # forever. Answer NO -- safe mode changes renderer settings, which would
    # silently invalidate everything the capture is about to measure.
    $dlgs = [CodVrWin]::Dialogs([uint32]$pid_)
    foreach ($d in $dlgs) {
        $parts = $d -split '\|', 2
        $h = [IntPtr][int64]$parts[0]
        Write-Host "  dialog: '$($parts[1])' -- answering No" -ForegroundColor Yellow
        # IDNO = 7, IDCANCEL = 2. WM_COMMAND with IDNO, then a close as backup.
        [void][CodVrWin]::PostMessageA($h, 0x0111, [IntPtr]7, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
        [void][CodVrWin]::PostMessageA($h, 0x0111, [IntPtr]2, [IntPtr]::Zero)
    }
    return $dlgs.Count
}

# ------------------------------------------------------------------ build
if ($Collect) { $SkipBuild = $true }
if (-not $SkipBuild) {
    Write-Host "[flat_run] building" -ForegroundColor Cyan
    & (Join-Path $root 'build.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

# ---------------------------------------------------------------- no leftovers
if (-not $NoLaunch) {
Get-Process CoDSP, CoDMP -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  killing leftover $($_.ProcessName) pid $($_.Id)" -ForegroundColor Yellow
    $_.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 800
    if (-not $_.HasExited) { $_.Kill() }
}
}

if ($Collect) {
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $got = @()
    foreach ($f in @('codvr_result.log', 'codvr.cfg')) {
        $p2 = Join-Path $gameDir $f
        if (Test-Path $p2) { Copy-Item $p2 $runDir -Force; $got += $f }
    }
    foreach ($pat in 'codvr_draws_*.csv', 'codvr_shot_*.tga') {
        Get-ChildItem $gameDir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $runDir -Force; $got += $_.Name
        }
    }
    $cl = Join-Path $gameDir 'Main\console.log'
    if (Test-Path $cl) { Copy-Item $cl $runDir -Force; $got += 'console.log' }
    # -Collect previously archived the armed cfg but left it (and the capture
    # DLL) deployed -- the other half of the 2026-08-25 armed-rig incident.
    $shippedCfg = Join-Path $dist 'codvr.cfg'
    if (Test-Path $shippedCfg) {
        Copy-Item $shippedCfg (Join-Path $gameDir 'codvr.cfg') -Force
        Write-Host "  codvr.cfg disarmed (shipped config restored)" -ForegroundColor DarkGray
    }
    Restore-SafeDeployment
    Write-Host "[flat_run] collected $($got.Count) file(s) into $runDir" -ForegroundColor Green
    $got | Sort-Object | ForEach-Object { Write-Host "    $_" }
    exit 0
}

# ------------------------------------------------------------------- config
# The shipped config is the base; the run's lines are APPENDED, and the
# parser's last-wins behaviour makes them overrides. That way a probe run
# differs from the deployed configuration by exactly the lines under test.
$cfg = Get-Content (Join-Path $dist 'codvr.cfg') -Raw
$cfg += "`r`n// ---- flat_run overrides: $Tag ----`r`n"
# The DLL ignores every probe cvar unless the config arms them first.
# See src/vr_cfg_contract.h: this line is what separates an attended
# capture run from the build a player launches.
$cfg += "vr_probe_arm 1`r`n"
$cfg += "vr_disable 1`r`n"                       # never create an XR session
foreach ($l in $CfgLines) { $cfg += "$l`r`n" }
$cfg += "vr_glcap_tag $Tag`r`n"
$cfg += "vr_glcap_autoquit 1`r`n"

Copy-Item (Join-Path $dist 'opengl32.dll') (Join-Path $gameDir 'opengl32.dll') -Force
Copy-Item (Join-Path $dist 'openxr_loader.dll') (Join-Path $gameDir 'openxr_loader.dll') -Force
Set-Content -Path (Join-Path $gameDir 'codvr.cfg') -Value $cfg -Encoding ascii

# The engine's own autoexec is the second way to drive it, used when the
# command line proves unreliable. Always restored, even on failure.
$autoexec       = Join-Path $gameDir 'Main\autoexec.cfg'
$autoexecBackup = Join-Path $gameDir 'Main\autoexec.cfg.flatrun-backup'
$autoexecTouched = $false
if ($AutoexecLines.Count -gt 0) {
    if (-not (Test-Path $autoexecBackup)) { Copy-Item $autoexec $autoexecBackup -Force }
    $autoexecTouched = $true
    $ae = Get-Content $autoexec -Raw
    $ae += "`r`n// ---- flat_run $Tag ----`r`n"
    foreach ($l in $AutoexecLines) { $ae += "$l`r`n" }
    Set-Content -Path $autoexec -Value $ae -Encoding ascii
}

# stale artefacts must not be mistaken for this run's output
Remove-Item (Join-Path $gameDir 'codvr_result.log') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $gameDir 'codvr_ingame.marker') -ErrorAction SilentlyContinue
Get-ChildItem $gameDir -Filter 'codvr_draws_*.csv' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem $gameDir -Filter 'codvr_shot_*.tga' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

# -------------------------------------------------------------------- launch
Write-Host "[flat_run] launching '$Tag' on map $Map (timeout ${TimeoutSec}s)" -ForegroundColor Cyan
if ($DeployOnly) {
    Write-Host ""
    Write-Host "  >>> DEPLOYED: $Tag" -ForegroundColor Green
    Write-Host "  >>> Start Call of Duty whenever you like, get into a level with a" -ForegroundColor Yellow
    Write-Host "  >>> weapon in hand, then STAND STILL for ~15 seconds." -ForegroundColor Yellow
    Write-Host "  >>> The mod runs the sweep and closes the game itself." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  collect afterwards with:  tools/flat_run.ps1 -Tag $Tag -Collect" -ForegroundColor DarkGray
    exit 0
}

$sw = [Diagnostics.Stopwatch]::StartNew()
if ($NoLaunch) {
    Write-Host ""
    Write-Host "  >>> DEPLOYED. Start Call of Duty yourself now (any way you like)," -ForegroundColor Yellow
    Write-Host "  >>> get into a level with a weapon in hand, then STAND STILL." -ForegroundColor Yellow
    Write-Host "  >>> The sweep runs itself and closes the game when it is done." -ForegroundColor Yellow
    Write-Host ""
} else {
    Remove-Item (Join-Path $gameDir 'Main\console.log') -ErrorAction SilentlyContinue
    $args = @('-applaunch', '2620', '+set', 'com_maxfps', '60', '+set', 'logfile', '2')
    $args += $ExtraArgs
    if ($Map) { $args += @('+spmap', $Map) }
    Write-Host "  args: $($args -join ' ')" -ForegroundColor DarkGray
    Start-Process -FilePath $steam -ArgumentList $args | Out-Null
}

$proc = $null
while ($sw.Elapsed.TotalSeconds -lt ($(if ($NoLaunch) { $TimeoutSec } else { 90 })) -and -not $proc) {
    Start-Sleep -Milliseconds 500
    $proc = Get-Process CoDSP -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $proc) { throw 'CoDSP.exe never started' }
Write-Host "  CoDSP.exe pid $($proc.Id)" -ForegroundColor Green
Write-Host ""
Write-Host "  >>> START THE LEVEL: click through the mission briefing, then leave" -ForegroundColor Yellow
Write-Host "  >>> the controls alone. The sweep runs itself and quits the game." -ForegroundColor Yellow
Write-Host ""

$ingameMarker    = Join-Path $gameDir 'codvr_ingame.marker'
$announcedIngame = $false
$expectPaths = @($Expect | ForEach-Object { Join-Path $gameDir $_ })
$sawExpected = $false
$forced = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 1000
    # ATTENDED BY DESIGN.
    #
    # Two things stop an unattended run from reaching gameplay: CoD1 SP does
    # not render the 3D scene unless its window is foreground, and `spmap`
    # lands on the mission-briefing menu waiting for a key. The obvious fix --
    # SetForegroundWindow plus synthetic keys -- is NOT used, because
    # SetForegroundWindow from a background shell is refused by Windows
    # foreground-lock often enough that the keystrokes would land in whatever
    # the user is actually doing. A person clicking Start once is both safer
    # and faster than a harness that sometimes types into their session.
    #
    # So the harness deploys, launches, and then WAITS for the mod's own
    # in-game marker. Everything after that is automatic.
    [void](Dismiss-GameDialogs $proc.Id)
    if (-not $announcedIngame -and (Test-Path $ingameMarker)) {
        $announcedIngame = $true
        Write-Host "  gameplay reached at $([int]$sw.Elapsed.TotalSeconds)s -- probe sweep running" -ForegroundColor Green
    }
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($expectPaths.Count -gt 0) {
        $have = @($expectPaths | Where-Object { Test-Path $_ })
        if ($have.Count -eq $expectPaths.Count -and -not $sawExpected) {
            $sawExpected = $true
            Write-Host "  all expected artefacts present at $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor Green
        }
    }
}
if (-not $proc.HasExited) {
    Write-Host "  TIMEOUT -- closing" -ForegroundColor Yellow
    $proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 5
    $proc.Refresh()
    if (-not $proc.HasExited) { $forced = $true; $proc.Kill(); Start-Sleep -Seconds 2 }
}
Write-Host "  exited after $([int]$sw.Elapsed.TotalSeconds)s (forced=$forced)" -ForegroundColor Cyan

# ------------------------------------------------------------------- collect
if ($autoexecTouched -and (Test-Path $autoexecBackup)) {
    Copy-Item $autoexecBackup $autoexec -Force
    Remove-Item $autoexecBackup -Force
    Write-Host "  autoexec.cfg restored" -ForegroundColor DarkGray
}
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Get-ChildItem $runDir -File | Remove-Item -Force -ErrorAction SilentlyContinue
$collected = @()
foreach ($f in @('codvr_result.log', 'codvr.cfg')) {
    $p = Join-Path $gameDir $f
    if (Test-Path $p) { Copy-Item $p $runDir -Force; $collected += $f }
}
Get-ChildItem $gameDir -Filter 'codvr_draws_*.csv' -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $runDir -Force; $collected += $_.Name
}
Get-ChildItem $gameDir -Filter 'codvr_shot_*.tga' -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $runDir -Force; $collected += $_.Name
}
# DISARM. The run's codvr.cfg has been archived above, so the game folder can
# go back to the shipped configuration. A flat capture rig must never be
# what is sitting there the next time the game is started for real.
$shippedCfg = Join-Path $dist 'codvr.cfg'
if (Test-Path $shippedCfg) {
    Copy-Item $shippedCfg (Join-Path $gameDir 'codvr.cfg') -Force
    Write-Host "  codvr.cfg disarmed (shipped config restored)" -ForegroundColor DarkGray
}
Restore-SafeDeployment

$conLog = Join-Path $gameDir 'Main\console.log'
if (Test-Path $conLog) { Copy-Item $conLog $runDir -Force; $collected += 'console.log' }
Write-Host "[flat_run] collected into $runDir :" -ForegroundColor Green
$collected | ForEach-Object { Write-Host "    $_" }

$missing = @($Expect | Where-Object { -not (Test-Path (Join-Path $runDir $_)) })
if ($missing.Count -gt 0) {
    Write-Host "[flat_run] MISSING expected artefacts: $($missing -join ', ')" -ForegroundColor Red
    exit 2
}
Write-Host "[flat_run] ok" -ForegroundColor Green
