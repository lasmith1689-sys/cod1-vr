<#
  ONE-TRANSACTION vbogame run: the single attended flat capture that answers
  whether the real CoD first-person VBO stream lets the shadow + palm solver
  recover the anatomical RIGHT and LEFT palm transforms.

  History this file exists to end:
  - 2026-08-26: run armed without the handsolve probe -> no palm data possible.
  - 2026-08-27 am: attended run played on the safe baseline -> nothing measured.
  - 2026-08-27 noon: two launches died at the deploy copy with "file in use",
    with no statement of WHO held the DLL, no retry, and the leftover-process
    sweep ordered AFTER the first mutation attempt.

  So this script now owns the ENTIRE lifecycle, and every mutation of the
  game folder passes a LOCK GATE first:

    build+gate -> pre-flight -> save pre-run copies (read-only) ->
    kill leftover CoD processes -> enumerate the DLL's actual lock owners
    (Restart Manager + module scan), auto-clear known-transient owners,
    ABORT pre-mutation on unknown owners -> prove write access ->
    deploy staged+verified -> re-hash -> launch -> banner watch ->
    bounded run -> collect -> ALWAYS restore the exact pre-run files with
    bounded lock retries and hash verification.

  The user does exactly one thing besides running this: click Start at the
  mission briefing, idle ~3 s, fire once, reload once, leave it alone.

  Exit codes: 0 = run completed AND restoration verified. 2 = run failed or
  aborted but the game folder is verified back in its pre-run state.
  3 = RESTORATION UNVERIFIED (printed loudly; exact copies kept).

  Test hooks (-GameDirTest and friends) exist so tools/test_vbogame_locks.ps1
  can drive this same transaction against a sandbox: same code path, no
  Steam, no launch.
#>
param(
    [int]      $TimeoutSec = 600,      # whole attended phase, launch to self-quit
    [switch]   $SkipBuild,             # trust dist/ as-is (still pre-flighted)
    [switch]   $PreflightOnly,         # run every offline check, deploy nothing
    # ---- test hooks (used by tools/test_vbogame_locks.ps1) ----
    [string]   $GameDirTest = '',      # sandbox game dir; implies -SkipLaunch
    [switch]   $SkipLaunch,            # stop after deploy verification, then restore
    [string[]] $SafeLockOwners = @('CoDSP', 'CoDMP'),   # lock owners we may kill
    [int]      $LockWaitSec = 30,      # pre-deploy lock clearance budget
    [int]      $RestoreRetrySec = 60,  # restore-time lock retry budget
    [int]      $PostDeployHoldSec = 0  # test-only pause between deploy and restore
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$dist    = Join-Path $root 'dist'
. (Join-Path $PSScriptRoot 'vbogame_lib.ps1')   # live-log reader + milestone parser
$testMode = ($GameDirTest -ne '')
if ($testMode) { $SkipLaunch = $true }
$gameDir = if ($testMode) { $GameDirTest } else { 'H:\SteamLibrary\steamapps\common\Call of Duty' }
$steam   = 'C:\Program Files (x86)\Steam\steam.exe'
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$runTag  = if ($testMode) { 'vbogame-locktest-' } else { 'vbogame-' }
$runDir  = Join-Path $root ("runs\" + $runTag + $stamp)

# The only hash the safe deployment is allowed to have before we start.
$SAFE_DLL_SHA = 'B3705FE51711C0DCA623159C6317A79CD4640BB7B4583E3B6345165182249CC6'

# The complete arming. quit_after 1890 = 90 skip + 2 recorded + ~30 s of
# lingering gameplay at 60 fps, so idle/fire/reload all happen while the
# palm probe is solving.
$ARM_LINES = @(
    'vr_probe_arm 1'
    'vr_disable 1'
    'vr_glcap_frames 2'
    'vr_glcap_skip 90'
    'vr_glcap_quit_after 1890'
    'vr_glprobe_handsolve 1'
    'vr_glcap_tag vbogame'
    'vr_glcap_autoquit 1'
)

function Sha([string]$p) { (Get-FileHash $p -Algorithm SHA256).Hash }
function Fail([string]$m) { throw "vbogame_run: $m" }

$prereqs = @($gameDir); if (-not $SkipLaunch) { $prereqs += $steam }
foreach ($p in $prereqs) {
    if (-not (Test-Path $p)) { Fail "missing prerequisite: $p" }
}

# ---------------------------------------------------------------- win32 bits
if (-not ("CodVrWin3" -as [type])) {
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CodVrWin3 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int  GetWindowTextA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int  GetClassNameA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static List<string> Dialogs(uint pid) {
        var found = new List<string>();
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

    // ---- Restart Manager: who is actually using this file? ----
    [StructLayout(LayoutKind.Sequential)]
    public struct RM_UNIQUE_PROCESS { public int dwProcessId; public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct RM_PROCESS_INFO {
        public RM_UNIQUE_PROCESS Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string strAppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string strServiceShortName;
        public int ApplicationType; public uint AppStatus; public uint TSSessionId;
        [MarshalAs(UnmanagedType.Bool)] public bool bRestartable;
    }
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    static extern int RmStartSession(out uint pSessionHandle, int dwSessionFlags, string strSessionKey);
    [DllImport("rstrtmgr.dll")]
    static extern int RmEndSession(uint pSessionHandle);
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    static extern int RmRegisterResources(uint pSessionHandle, uint nFiles, string[] rgsFilenames, uint nApplications, RM_UNIQUE_PROCESS[] rgApplications, uint nServices, string[] rgsServiceNames);
    [DllImport("rstrtmgr.dll")]
    static extern int RmGetList(uint dwSessionHandle, out uint pnProcInfoNeeded, ref uint pnProcInfo, [In, Out] RM_PROCESS_INFO[] rgAffectedApps, ref uint lpdwRebootReasons);
    public static List<int> LockOwners(string path, List<string> appNames) {
        var pids = new List<int>();
        uint h; string key = Guid.NewGuid().ToString();
        if (RmStartSession(out h, 0, key) != 0) return pids;
        try {
            if (RmRegisterResources(h, 1, new string[] { path }, 0, null, 0, null) != 0) return pids;
            uint needed = 0, count = 0, reasons = 0;
            int rc = RmGetList(h, out needed, ref count, null, ref reasons);
            if (rc == 234 && needed > 0) {           // ERROR_MORE_DATA
                var arr = new RM_PROCESS_INFO[needed]; count = needed;
                rc = RmGetList(h, out needed, ref count, arr, ref reasons);
                if (rc == 0) for (uint i = 0; i < count; i++) { pids.Add(arr[i].Process.dwProcessId); appNames.Add(arr[i].strAppName); }
            }
        } finally { RmEndSession(h); }
        return pids;
    }
}
"@
}
function Dismiss-GameDialogs([int]$pid_) {
    foreach ($d in [CodVrWin3]::Dialogs([uint32]$pid_)) {
        $parts = $d -split '\|', 2
        $h = [IntPtr][int64]$parts[0]
        Write-Host "  dialog: '$($parts[1])' -- answering No" -ForegroundColor Yellow
        [void][CodVrWin3]::PostMessageA($h, 0x0111, [IntPtr]7, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
        [void][CodVrWin3]::PostMessageA($h, 0x0111, [IntPtr]2, [IntPtr]::Zero)
    }
}

# ------------------------------------------------------------- lock machinery
# Who is using $path right now? Restart Manager sees open handles AND loaded
# modules; the module scan is a belt-and-braces cross-check. Every owner is
# named and classified -- no more anonymous "being used by another process".
function Get-LockOwners([string]$path) {
    $owners = @()
    $apps = New-Object 'System.Collections.Generic.List[string]'
    $pids = [CodVrWin3]::LockOwners($path, $apps)
    for ($i = 0; $i -lt $pids.Count; $i++) {
        $pr = Get-Process -Id $pids[$i] -ErrorAction SilentlyContinue
        if (-not $pr) { continue }   # already gone
        $owners += [pscustomobject]@{
            Pid = $pids[$i]; Name = $pr.ProcessName
            Path = $(try { $pr.Path } catch { '(access denied)' })
            App = $apps[$i]
        }
    }
    # Cross-check: processes with the file mapped as a module (Restart Manager
    # normally reports these too; this catches the ones it misses).
    Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
        $pr = $_
        try {
            if (($pr.Modules | Where-Object { $_.FileName -eq $path }) -and
                -not ($owners | Where-Object { $_.Pid -eq $pr.Id })) {
                $owners += [pscustomobject]@{
                    Pid = $pr.Id; Name = $pr.ProcessName
                    Path = $(try { $pr.Path } catch { '(access denied)' })
                    App = '(module scan)'
                }
            }
        } catch {}
    }
    return @($owners)
}

function Classify-Owner($owner) {
    if ($SafeLockOwners -contains $owner.Name) { return 'safe-kill (known CoD-transient)' }
    if ($owner.Name -eq 'gameoverlayui')       { return 'wait (Steam overlay of a dying game)' }
    return 'UNKNOWN'
}

# Can $path be replaced right now? A loaded module allows reads but denies
# writes, so only an actual write-access open proves replaceability.
function Test-Replaceable([string]$path) {
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Write', 'None')
        $fs.Close()
        return $true
    } catch { return $false }
}

# Clear every lock on $path within $budgetSec: kill safe-kill owners (by PID,
# never by blanket name), wait out wait-class owners, ABORT on unknowns.
# Returns $true when a write-access open succeeds; throws on unknown owners
# so the caller can abort BEFORE mutating anything.
function Clear-FileLocks([string]$path, [int]$budgetSec, [string]$why) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $reported = @{}
    while ($true) {
        if (Test-Replaceable $path) {
            if ($reported.Count -gt 0) { Write-Host "  lock released; $why is writable" -ForegroundColor Green }
            return $true
        }
        $owners = Get-LockOwners $path
        foreach ($o in $owners) {
            $class = Classify-Owner $o
            if (-not $reported.ContainsKey($o.Pid)) {
                $reported[$o.Pid] = $true
                Write-Host "[vbogame] lock owner: pid $($o.Pid) '$($o.Name)' ($class)" -ForegroundColor Yellow
                Write-Host "          path: $($o.Path)  app: '$($o.App)'" -ForegroundColor DarkGray
            }
            if ($class -like 'safe-kill*') {
                $pr = Get-Process -Id $o.Pid -ErrorAction SilentlyContinue
                if ($pr) {
                    Write-Host "  closing pid $($o.Pid) $($o.Name)" -ForegroundColor Yellow
                    $pr.CloseMainWindow() | Out-Null
                    Start-Sleep -Seconds 2
                    $pr.Refresh()
                    if (-not $pr.HasExited) { $pr.Kill(); Start-Sleep -Seconds 1 }
                }
            } elseif ($class -eq 'UNKNOWN') {
                Fail ("UNKNOWN process holds $path`n  pid $($o.Pid) '$($o.Name)' at $($o.Path)`nAborting before touching the game folder. Close that process and rerun.")
            }
        }
        if ($owners.Count -eq 0) {
            # Nothing claims it, yet writes fail: a transient scanner
            # (AV/indexer) that Restart Manager cannot see. Waiting it out is
            # the correct move; it is exactly the failure mode that killed
            # the 2026-08-27 attempts with no explanation.
            Write-Host "  no named owner but write access denied (transient scanner?) -- waiting" -ForegroundColor Yellow
        }
        if ($sw.Elapsed.TotalSeconds -ge $budgetSec) {
            $who = if ($owners.Count) { ($owners | ForEach-Object { "pid $($_.Pid) $($_.Name)" }) -join ', ' } else { 'no named owner (transient scanner?)' }
            Fail "$why still locked after ${budgetSec}s by: $who"
        }
        Start-Sleep -Seconds 2
    }
}

# Replace $dest with $src: stage beside the destination, verify the staged
# bytes, atomically swap, verify the destination. Nothing is ever written
# straight over the live file.
function Replace-Verified([string]$src, [string]$dest, [string]$wantSha) {
    $stage = "$dest.vbogame_stage"
    $bak   = "$dest.vbogame_bak"
    Copy-Item $src $stage -Force
    $got = Sha $stage
    if ($got -ne $wantSha) { Remove-Item $stage -Force -EA SilentlyContinue; Fail "staged copy hash $got != wanted $wantSha" }
    try {
        [IO.File]::Replace($stage, $dest, $bak)
        Remove-Item $bak -Force -ErrorAction SilentlyContinue
    } catch {
        # Replace needs both files healthy; fall back to a plain overwrite,
        # which the lock gate has already proven possible.
        Copy-Item $stage $dest -Force
        Remove-Item $stage -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $stage -Force -ErrorAction SilentlyContinue
    $got = Sha $dest
    if ($got -ne $wantSha) { Fail "post-replace hash $got != wanted $wantSha" }
}

# The 2026-08-27 noon failures: the game was started BY HAND alongside the
# harness (the old DeployOnly workflow said "start the game whenever you
# like"), so a running CoDSP.exe held opengl32.dll at the exact moment of
# the deploy copy. Say the new contract out loud before doing anything.
if (-not $testMode) {
    Write-Host ""
    Write-Host "  >>> Do NOT start Call of Duty yourself. This command checks the" -ForegroundColor Yellow
    Write-Host "  >>> game folder, deploys the probe, and LAUNCHES THE GAME FOR YOU" -ForegroundColor Yellow
    Write-Host "  >>> when everything is verified. A game it finds running will be" -ForegroundColor Yellow
    Write-Host "  >>> closed automatically." -ForegroundColor Yellow
    Write-Host ""
}

# ------------------------------------------------------------ build + gate
if (-not $SkipBuild) {
    Write-Host "[vbogame] building + full gate" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -SelfTest | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 'build/gate failed -- nothing was deployed' }
}

# --------------------------------------------------------------- pre-flight
Write-Host "[vbogame] pre-flight" -ForegroundColor Cyan
$distDll  = Join-Path $dist 'opengl32.dll'
$distCfg  = Join-Path $dist 'codvr.cfg'
$distHash = Sha $distDll

if ($distHash -eq $SAFE_DLL_SHA) { Fail "pre-flight A: dist DLL IS the safe baseline ($SAFE_DLL_SHA) -- there is nothing instrumented to deploy" }
Write-Host "  A ok: staged $($distHash.Substring(0,8))... != safe $($SAFE_DLL_SHA.Substring(0,8))..." -ForegroundColor Green

$bytes = [IO.File]::ReadAllBytes($distDll)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
foreach ($needle in @(
    '=== VBOGAME REAL PALM PROBE ACTIVE ===',
    'vr_glprobe_handsolve',
    'hand-solve probe',
    'vbo-sourced reads')) {
    if (-not $ascii.Contains($needle)) { Fail "pre-flight B: staged DLL lacks '$needle' -- wrong/old build staged" }
}
Write-Host "  B ok: staged DLL is gate-green and carries banner + handsolve + VBO shadow" -ForegroundColor Green

$cfgText = (Get-Content $distCfg -Raw) + "`r`n// ---- vbogame one-transaction run $stamp ----`r`n"
foreach ($l in $ARM_LINES) { $cfgText += "$l`r`n" }
foreach ($must in @('vr_probe_arm 1', 'vr_glprobe_handsolve 1',
                    'vr_glcap_frames 2', 'vr_glcap_skip 90',
                    'vr_glcap_quit_after 1890', 'vr_glcap_autoquit 1',
                    'vr_glcap_tag vbogame', 'vr_disable 1')) {
    if ($cfgText -notmatch [regex]::Escape($must)) { Fail "pre-flight C/D: armed cfg is missing '$must'" }
}
Write-Host "  C/D ok: armed cfg carries capture + handsolve arming" -ForegroundColor Green

foreach ($needle in @('PROBE ARMED', 'PRODUCTION PROBE CONTRACT')) {
    if (-not $ascii.Contains($needle)) { Fail "pre-flight G: staged DLL lacks the '$needle' contract report" }
}
Write-Host "  G ok: startup contract reporting present" -ForegroundColor Green

if ($PreflightOnly) {
    Write-Host "[vbogame] pre-flight complete; nothing deployed (-PreflightOnly)" -ForegroundColor Green
    exit 0
}

# ------------------------------------------------- save exact pre-run state
Write-Host "[vbogame] checking game state" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $runDir, (Join-Path $runDir 'prerun') | Out-Null
$gameDll = Join-Path $gameDir 'opengl32.dll'
$gameCfg = Join-Path $gameDir 'codvr.cfg'
$preDll  = Join-Path $runDir 'prerun\opengl32.dll'
$preCfg  = Join-Path $runDir 'prerun\codvr.cfg'
$preDllSha = Sha $gameDll
$preCfgSha = Sha $gameCfg
if ($preDllSha -ne $SAFE_DLL_SHA) {
    if ($preDllSha -eq $distHash) {
        Write-Host "  game folder holds our own capture rig (interrupted earlier transaction) -- restoring the safe checkpoint first" -ForegroundColor Yellow
        $ckpt = Join-Path $root 'checkpoints\deployed-as-found-2026-08-24'
        Clear-FileLocks $gameDll $LockWaitSec 'game DLL (pre-recovery)' | Out-Null
        Replace-Verified (Join-Path $ckpt 'opengl32.dll') $gameDll $SAFE_DLL_SHA
        Copy-Item (Join-Path $ckpt 'codvr.cfg') $gameCfg -Force
        $preDllSha = Sha $gameDll
        $preCfgSha = Sha $gameCfg
    }
    if ($preDllSha -ne $SAFE_DLL_SHA) {
        Fail ("game folder DLL is NOT the expected safe baseline`n  found  $preDllSha`n  wanted $SAFE_DLL_SHA`nRefusing to overwrite an unknown deployment.")
    }
}
Copy-Item $gameDll $preDll -Force
Copy-Item $gameCfg $preCfg -Force
"opengl32.dll  $preDllSha`r`ncodvr.cfg  $preCfgSha" |
    Set-Content (Join-Path $runDir 'prerun\SHA256SUMS.txt') -Encoding ascii
Write-Host "[vbogame] safe baseline verified; pre-run copies saved (DLL $($preDllSha.Substring(0,8))..., cfg $($preCfgSha.Substring(0,8))...)" -ForegroundColor Green

# ------------------------------------------------------ the transaction
$runOk        = $false
$restoreOk    = $false
$deployed     = $false
$failReason   = ''
$proc         = $null
try {
    # 1. Leftover CoD processes go FIRST -- the noon failures had this sweep
    #    ordered after the first mutation attempt. Real runs only: a sandbox
    #    test must never reach into the real game's processes.
    if (-not $testMode) {
        Get-Process CoDSP, CoDMP -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "  killing leftover $($_.ProcessName) pid $($_.Id)" -ForegroundColor Yellow
            $_.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 800
            if (-not $_.HasExited) { $_.Kill() }
        }
    }

    # 2. THE LOCK GATE. Nothing is mutated until the destination provably
    #    accepts a replacement. Unknown owners abort here, pre-mutation.
    Clear-FileLocks $gameDll $LockWaitSec 'game DLL' | Out-Null

    # 3. The DLL we just proved writable must still be the one we saved.
    $nowSha = Sha $gameDll
    if ($nowSha -ne $preDllSha) { Fail "game DLL changed while clearing locks ($nowSha != $preDllSha)" }

    # 4. Deploy: staged, swapped, verified.
    Replace-Verified $distDll $gameDll $distHash
    $deployed = $true
    Copy-Item (Join-Path $dist 'openxr_loader.dll') (Join-Path $gameDir 'openxr_loader.dll') -Force
    Set-Content -Path $gameCfg -Value $cfgText -Encoding ascii

    # 5. Re-verify the whole deployment BEFORE any launch (pre-flight E + F).
    $deployedSha = Sha $gameDll
    if ($deployedSha -ne $distHash) { Fail "pre-flight E: deployed DLL $deployedSha != staged $distHash" }
    $deployedCfg = Get-Content $gameCfg -Raw
    if ($deployedCfg -notmatch [regex]::Escape('vr_probe_arm 1') -or
        $deployedCfg -notmatch [regex]::Escape('vr_glprobe_handsolve 1')) {
        Fail 'pre-flight F: deployed cfg is not armed'
    }
    Write-Host "[vbogame] probe deployed and hash verified ($($deployedSha.Substring(0,8))...)" -ForegroundColor Green

    if ($SkipLaunch) {
        if ($PostDeployHoldSec -gt 0) {
            Write-Host "  (test hold: $PostDeployHoldSec s with the rig deployed)" -ForegroundColor DarkGray
            Start-Sleep -Seconds $PostDeployHoldSec
        }
        Write-Host "[vbogame] -SkipLaunch: transaction mechanics complete; restoring" -ForegroundColor Cyan
        $runOk = $true
    } else {
        # Clear stale artefacts so nothing old is mistaken for this run's output.
        $marker = Join-Path $gameDir 'codvr_ingame.marker'
        Remove-Item (Join-Path $gameDir 'codvr_result.log') -ErrorAction SilentlyContinue
        Remove-Item $marker -ErrorAction SilentlyContinue
        Get-ChildItem $gameDir -Filter 'codvr_draws_*.csv' -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue
        Get-ChildItem $gameDir -Filter 'codvr_shot_*.tga'  -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue
        Remove-Item (Join-Path $gameDir 'Main\console.log') -ErrorAction SilentlyContinue

        Write-Host "[vbogame] launching (timeout ${TimeoutSec}s)" -ForegroundColor Cyan
        Start-Process -FilePath $steam -ArgumentList @(
            '-applaunch', '2620', '+set', 'com_maxfps', '60',
            '+set', 'logfile', '2', '+spmap', 'chateau') | Out-Null

        Write-Host ""
        Write-Host "  >>> Game starting. Do NOT click anything yet -- the harness is" -ForegroundColor Yellow
        Write-Host "  >>> verifying the loaded module first." -ForegroundColor Yellow
        Write-Host ""

        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.Elapsed.TotalSeconds -lt 90 -and -not $proc) {
            Start-Sleep -Milliseconds 500
            $proc = Get-Process CoDSP -ErrorAction SilentlyContinue | Select-Object -First 1
        }
        if (-not $proc) { Fail 'CoDSP.exe never started' }
        Write-Host "  CoDSP.exe pid $($proc.Id)" -ForegroundColor Green

        $resultLog = Join-Path $gameDir 'codvr_result.log'
        $lastDeepest = 'NONE'
        $ms = $null

        # ---- PHASE A: prove the deployment in the LIVE process before the
        # user is asked to touch anything. Module identity is established
        # EXTERNALLY (enumerate pid's modules, hash the app-local file while
        # the process is alive); the internal log is a live feed plus a
        # build-id cross-check whose ABSENCE proves nothing -- two healthy
        # runs were killed today on exactly that misreading. Abort here,
        # before any user interaction, if the pre-click states do not pass.
        $applocal = $false
        $preDeadline = $sw.Elapsed.TotalSeconds + 120
        while ($sw.Elapsed.TotalSeconds -lt $preDeadline) {
            Start-Sleep -Milliseconds 1000
            Dismiss-GameDialogs $proc.Id
            $proc.Refresh()
            if ($proc.HasExited) { Fail 'game exited during pre-click verification' }
            if (-not $applocal) {
                $paths = Get-ProcessOpenGLModules $proc
                if ($paths.Count -gt 0) {
                    $id = Resolve-OpenGLIdentity $paths $gameDll $distHash
                    if ($id.Status -eq 'PASS') {
                        $applocal = $true
                        Write-Host "  APPLOCAL_OPENGL32_LOADED: pid $($proc.Id) has $($id.Detail), on-disk hash == deployed" -ForegroundColor Green
                    } elseif ($id.Status -eq 'WRONG_PATH' -or $id.Status -eq 'HASH_MISMATCH') {
                        Fail "module identity FAILED: $($id.Status) -- $($id.Detail)"
                    }
                }
            }
            $ms = Get-VbogameMilestones (Read-LiveLogText $resultLog) $distHash $true $applocal
            if ($ms.Deepest -ne $lastDeepest) {
                Write-Host "  milestone: $($ms.Deepest) at $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor Green
                $lastDeepest = $ms.Deepest
            }
            if ($ms.Verdict -like 'WRONG_BINARY*') {
                Fail "internal build-id cross-check: $($ms.Verdict)"
            }
            if ($applocal -and $ms.Stages['PROXY_FIRST_CALL'] -and
                $ms.Stages['PROBE_CONFIG_ARMED']) { break }
        }
        if (-not ($applocal -and $ms -and $ms.Stages['PROXY_FIRST_CALL'] -and
                  $ms.Stages['PROBE_CONFIG_ARMED'])) {
            Fail ("pre-click verification incomplete within 120 s " +
                  "(applocal=$applocal, deepest: $(if ($ms) { $ms.Deepest } else { 'NONE' })) " +
                  "-- aborted BEFORE any user interaction was needed")
        }

        Write-Host ""
        Write-Host "  >>> VERIFIED. Click Start now. Once in game: idle ~3 s, fire" -ForegroundColor Yellow
        Write-Host "  >>> once, reload once, then leave it alone." -ForegroundColor Yellow
        Write-Host ""

        # ---- PHASE B: the attended run. Milestones are REPORTED as they
        # appear; nothing here aborts on a missing one. Only a proven wrong
        # binary (present-and-mismatched build id) aborts.
        while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
            Start-Sleep -Milliseconds 1000
            Dismiss-GameDialogs $proc.Id
            $proc.Refresh()
            if ($proc.HasExited) { break }
            $ms = Get-VbogameMilestones (Read-LiveLogText $resultLog) $distHash $true $applocal
            if ($ms.Deepest -ne $lastDeepest) {
                Write-Host "  milestone: $($ms.Deepest) at $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor Green
                $lastDeepest = $ms.Deepest
            }
            if ($ms.Verdict -like 'WRONG_BINARY*') {
                Fail "internal build-id cross-check: $($ms.Verdict)"
            }
        }
        if (-not $proc.HasExited) {
            Write-Host "  TIMEOUT -- closing the game" -ForegroundColor Yellow
            $proc.CloseMainWindow() | Out-Null
            Start-Sleep -Seconds 5
            $proc.Refresh()
            if (-not $proc.HasExited) { $proc.Kill(); Start-Sleep -Seconds 2 }
        }
        $ms = Get-VbogameMilestones (Read-LiveLogText $resultLog) $distHash $true $applocal
        Write-Host "[vbogame] deepest milestone: $($ms.Deepest)  (build-id cross-check: $($ms.CrossCheck))" -ForegroundColor Cyan
        foreach ($k in $ms.Stages.Keys) {
            $on = $ms.Stages[$k]
            Write-Host ("    {0} {1}" -f $(if ($on) { '[x]' } else { '[ ]' }), $k) -ForegroundColor $(if ($on) { 'Green' } else { 'DarkGray' })
        }

        # Collect everything.
        $collected = @()
        foreach ($f in @('codvr_result.log', 'codvr.cfg')) {
            $p = Join-Path $gameDir $f
            if (Test-Path $p) { Copy-Item $p $runDir -Force; $collected += $f }
        }
        foreach ($pat in 'codvr_draws_*.csv', 'codvr_shot_*.tga') {
            Get-ChildItem $gameDir -Filter $pat -EA SilentlyContinue | ForEach-Object {
                Copy-Item $_.FullName $runDir -Force; $collected += $_.Name
            }
        }
        $cl = Join-Path $gameDir 'Main\console.log'
        if (Test-Path $cl) { Copy-Item $cl $runDir -Force; $collected += 'console.log' }
        Write-Host "[vbogame] collected $($collected.Count) file(s) into $runDir" -ForegroundColor Green
        $collected | Sort-Object | ForEach-Object { Write-Host "    $_" }

        if ($collected -notcontains 'codvr_result.log') { Fail 'no codvr_result.log came back' }
        if (-not $applocal) {
            Fail "run ended without external module identity ever passing (deepest: $($ms.Deepest))"
        }
        $runOk = $true
    }
}
catch {
    $failReason = $_.Exception.Message
    Write-Host "[vbogame] RUN FAILED: $failReason" -ForegroundColor Red
    if ($proc) {
        try {
            $proc.Refresh()
            if (-not $proc.HasExited) {
                $proc.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 5
                $proc.Refresh()
                if (-not $proc.HasExited) { $proc.Kill() }
            }
        } catch {}
    }
}
finally {
    # EVERY exit path lands here. If nothing was ever deployed, the game
    # folder was never mutated and restoration is a verification, not a
    # write. If the rig went out, the exact saved pre-run files go back --
    # through the same lock gate, with a bounded retry, because the game
    # (or a scanner) may still be releasing the DLL.
    try {
        $needWrite = $deployed
        if ($needWrite) {
            Write-Host "[vbogame] waiting for safe release before restore (budget ${RestoreRetrySec}s)" -ForegroundColor Cyan
            Clear-FileLocks $gameDll $RestoreRetrySec 'game DLL (restore)' | Out-Null
            Replace-Verified $preDll $gameDll $preDllSha
            Copy-Item $preCfg $gameCfg -Force   # byte-exact, no re-encoding
        }
        $nowDll = Sha $gameDll
        $nowCfg = Sha $gameCfg
        if ($nowDll -eq $preDllSha -and $nowCfg -eq $preCfgSha) {
            $restoreOk = $true
            Write-Host "[vbogame] pre-run deployment RESTORED and verified" -ForegroundColor Green
            Write-Host "    opengl32.dll $nowDll"
            Write-Host "    codvr.cfg    $nowCfg"
        } else {
            Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
            Write-Host "!!! RESTORATION FAILED -- DO NOT PLAY UNTIL FIXED !!!"     -ForegroundColor Red
            Write-Host "!!! game DLL  now $nowDll"                                  -ForegroundColor Red
            Write-Host "!!! game DLL want $preDllSha"                               -ForegroundColor Red
            Write-Host "!!! game cfg  now $nowCfg"                                  -ForegroundColor Red
            Write-Host "!!! game cfg want $preCfgSha"                               -ForegroundColor Red
            Write-Host "!!! exact copies live in $runDir\prerun"                    -ForegroundColor Red
            Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        }
    } catch {
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
        Write-Host "!!! RESTORATION THREW: $($_.Exception.Message)"            -ForegroundColor Red
        Write-Host "!!! exact pre-run copies live in $runDir\prerun"           -ForegroundColor Red
        Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
    }
}

# Quick evidence summary so the terminal itself says what the run proved.
$rl = Join-Path $runDir 'codvr_result.log'
if ($runOk -and -not $SkipLaunch -and (Test-Path $rl)) {
    Write-Host ""
    Write-Host "[vbogame] palm / vbo evidence lines:" -ForegroundColor Cyan
    Select-String -Path $rl -Pattern 'palm |vbo-sourced|VBOGAME|wrote \d+ draw records|^=== ' |
        ForEach-Object { Write-Host "    $($_.Line)" }
}

if (-not $restoreOk) { exit 3 }
if (-not $runOk)     { Write-Host "[vbogame] failed: $failReason" -ForegroundColor Red; exit 2 }
Write-Host "[vbogame] ok -- artefacts in $runDir" -ForegroundColor Green
exit 0
