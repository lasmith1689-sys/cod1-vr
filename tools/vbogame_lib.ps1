<#
  Shared pieces of the vbogame attended-run harness: EXTERNAL module
  identity, the live-log reader, and the milestone-protocol parser.
  Dot-sourced by vbogame_run.ps1 AND by tools/test_vbogame_milestones.ps1,
  so the tests exercise the exact code the harness runs.

  AUTHORITY ORDER (2026-08-27, after two watcher-blinded runs):
  1. Module identity is established EXTERNALLY: enumerate the live CoDSP
     process's modules, require an opengl32.dll whose on-disk path is the
     game folder's and whose file hashes to the deployed build. No internal
     log line is needed for that fact.
  2. The internal log is a live milestone feed and a build-id CROSS-CHECK.
     Its absence proves nothing (the 15:53 run: the logger held the file
     with fopen_s = sharing denied entirely, so no external reader of any
     share mode could open it while the game lived -- and the run itself
     was perfectly healthy). Only a PRESENT-and-MISMATCHED build id may be
     read as "wrong binary".
#>

function Read-LiveLogText([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    try {
        $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open,
                                       [IO.FileAccess]::Read,
                                       ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try {
            $sr = New-Object IO.StreamReader($fs)
            return $sr.ReadToEnd()
        } finally { $fs.Close() }
    } catch { return '' }
}

# Every opengl32.dll module the process has loaded (the proxy design means a
# healthy CoDSP has TWO: the app-local proxy and the real system one it
# forwards to). Returns file paths; empty array if none yet / not readable.
#
# EnumProcessModulesEx(LIST_MODULES_ALL), NOT .NET Process.Modules: the .NET
# property uses the non-Ex API, which cannot enumerate a 32-bit (WOW64)
# process from 64-bit PowerShell -- and CoDSP is 32-bit. The self-test's
# case A caught exactly that before it could blind a real run.
if (-not ("CodVrMods" -as [type])) {
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CodVrMods {
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool EnumProcessModulesEx(IntPtr h, [Out] IntPtr[] mods, uint cb, out uint needed, uint filter);
    [DllImport("psapi.dll", CharSet = CharSet.Unicode)]
    static extern uint GetModuleFileNameExW(IntPtr h, IntPtr mod, StringBuilder name, uint size);
    [DllImport("kernel32.dll")]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    public static List<string> ModulePaths(int pid) {
        var res = new List<string>();
        IntPtr h = OpenProcess(0x0400 | 0x0010, false, pid);   // QUERY_INFORMATION | VM_READ
        if (h == IntPtr.Zero) return res;
        try {
            var mods = new IntPtr[2048];
            uint needed;
            if (!EnumProcessModulesEx(h, mods, (uint)(mods.Length * IntPtr.Size), out needed, 0x03))
                return res;                                     // 0x03 = LIST_MODULES_ALL
            int count = (int)(needed / (uint)IntPtr.Size);
            if (count > mods.Length) count = mods.Length;
            var sb = new StringBuilder(1024);
            for (int i = 0; i < count; i++) {
                sb.Length = 0;
                if (GetModuleFileNameExW(h, mods[i], sb, 1024) > 0) res.Add(sb.ToString());
            }
        } finally { CloseHandle(h); }
        return res;
    }
}
"@
}
function Get-ProcessOpenGLModules($proc) {
    $paths = @()
    try {
        foreach ($p in [CodVrMods]::ModulePaths($proc.Id)) {
            if ((Split-Path $p -Leaf) -ieq 'opengl32.dll') { $paths += $p }
        }
    } catch {}
    return $paths
}

# The identity decision, pure enough to unit-test: given the loaded
# opengl32 paths, the expected app-local path and the deployed hash,
# say what is true. Hashing an image-mapped file is permitted by the
# loader (readers are not blocked), so this works while the game runs.
function Resolve-OpenGLIdentity([string[]]$modulePaths, [string]$expectedPath,
                                [string]$expectedSha) {
    if (-not $modulePaths -or $modulePaths.Count -eq 0) {
        return [pscustomobject]@{ Status = 'NOT_LOADED'; Detail = 'no opengl32.dll module yet' }
    }
    $hit = $modulePaths | Where-Object { $_ -ieq $expectedPath } | Select-Object -First 1
    if (-not $hit) {
        return [pscustomobject]@{ Status = 'WRONG_PATH'
                                  Detail = "loaded only: $($modulePaths -join '; ')" }
    }
    try { $got = (Get-FileHash $hit -Algorithm SHA256 -ErrorAction Stop).Hash }
    catch { return [pscustomobject]@{ Status = 'UNREADABLE'; Detail = $_.Exception.Message } }
    if ($got -ne $expectedSha) {
        return [pscustomobject]@{ Status = 'HASH_MISMATCH'
                                  Detail = "on-disk $got != deployed $expectedSha" }
    }
    return [pscustomobject]@{ Status = 'PASS'; Detail = $hit }
}

# The milestone ladder. APPLOCAL_OPENGL32_LOADED is established externally
# and INJECTED; everything below PROXY_FIRST_CALL comes from the log.
$script:VBOGAME_STAGES = @(
    'PROCESS_STARTED',
    'APPLOCAL_OPENGL32_LOADED',   # external module check (authoritative)
    'PROXY_FIRST_CALL',
    'PROBE_CONFIG_ARMED',
    'CGAME_HOOKED',
    'FIRST_3D_SCENE',
    'FIRST_VIEWMODEL_DRAW',
    'VBO_SHADOW_RESOLVED',
    'RIGHT_PALM_SOLVED',
    'LEFT_PALM_SOLVED',
    'REAL_PALM_PATH_ACTIVE'
)

function Get-VbogameMilestones([string]$logText, [string]$expectedSha,
                               [bool]$processStarted = $true,
                               [bool]$applocalLoaded = $false) {
    $m = [ordered]@{}
    foreach ($s in $script:VBOGAME_STAGES) { $m[$s] = $false }
    $m['PROCESS_STARTED'] = $processStarted
    $m['APPLOCAL_OPENGL32_LOADED'] = $applocalLoaded
    $verdict = ''            # only a PROVEN mismatch lands here
    $crossCheck = 'ABSENT'   # internal build id: ABSENT / MATCH / MISMATCH

    if ($logText -and $logText.Length -gt 0) {
        $idMatch = [regex]::Match($logText, 'build id/hash:\s*([0-9A-Fa-f]{64}|UNAVAILABLE)')
        if ($idMatch.Success) {
            if ($idMatch.Groups[1].Value -eq $expectedSha) { $crossCheck = 'MATCH' }
            elseif ($idMatch.Groups[1].Value -eq 'UNAVAILABLE') { $crossCheck = 'UNAVAILABLE' }
            else {
                $crossCheck = 'MISMATCH'
                $verdict = "WRONG_BINARY ($($idMatch.Groups[1].Value.Substring(0,8))... in-log)"
            }
        }
        if ($logText.Contains('=== PROXY FIRST CALL ==='))     { $m['PROXY_FIRST_CALL'] = $true }
        if ($logText.Contains('=== VBOGAME PROBE CONFIG ===') -and
            $logText.Contains('armed: YES') -and
            $logText.Contains('handsolve: YES'))               { $m['PROBE_CONFIG_ARMED'] = $true }
        if ($logText -match 'cgame @ [0-9A-Fa-f]+ hooked')     { $m['CGAME_HOOKED'] = $true }
        if ($logText.Contains('=== FIRST 3D SCENE ==='))       { $m['FIRST_3D_SCENE'] = $true }
        if ($logText.Contains('=== FIRST VIEWMODEL DRAW ===')) { $m['FIRST_VIEWMODEL_DRAW'] = $true }
        if ($logText.Contains('=== VBO SHADOW RESOLVED ==='))  { $m['VBO_SHADOW_RESOLVED'] = $true }
        if ($logText.Contains('=== RIGHT PALM SOLVED ==='))    { $m['RIGHT_PALM_SOLVED'] = $true }
        if ($logText.Contains('=== LEFT PALM SOLVED ==='))     { $m['LEFT_PALM_SOLVED'] = $true }
        if ($logText.Contains('=== REAL PALM PATH ACTIVE ===')) { $m['REAL_PALM_PATH_ACTIVE'] = $true }
    }

    $deepest = 'NONE'
    foreach ($s in $script:VBOGAME_STAGES) { if ($m[$s]) { $deepest = $s } }

    [pscustomobject]@{
        Stages     = $m
        Deepest    = $deepest
        Verdict    = $verdict
        CrossCheck = $crossCheck
    }
}
