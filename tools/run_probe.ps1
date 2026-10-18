<#
  Launch CoD1 straight into a map, wait for the DLL to write its verdict,
  then shut the game down.

  Overrides are passed on the command line rather than written to a .cfg so
  the user's own autoexec.cfg is left alone, and the process is killed rather
  than quit cleanly so the engine never rewrites config.cfg with our values.
#>
param(
    [string]$Map = 'chateau',
    [int]$TimeoutSeconds = 150,
    [int]$Width = 1280,
    [int]$Height = 720
)

$ErrorActionPreference = 'Stop'
$game = 'H:\SteamLibrary\steamapps\common\Call of Duty'
$log  = Join-Path $game 'codvr_spike3.log'

Get-Process CoDSP -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $log -ErrorAction SilentlyContinue

# Single-player levels load with spmap; plain `map` is the multiplayer path
# and leaves the game sitting at the menu. Video cvars are deliberately not
# overridden here -- combining r_mode -1 with custom width/height fought the
# user's autoexec and made the process exit immediately.
$args = @('+set', 'r_fullscreen', '0', '+spmap', $Map)

Write-Host "launching: CoDSP.exe $($args -join ' ')"
$proc = Start-Process -FilePath (Join-Path $game 'CoDSP.exe') `
                      -WorkingDirectory $game -ArgumentList $args -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$verdict  = $false

while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) { Write-Host "game exited early (code $($proc.ExitCode))"; break }
    if (Test-Path $log) {
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($text -match 'FRAMEBUFFER VERDICT' -and $text -match 'end call order') {
            $verdict = $true
            Write-Host 'verdict + trace present, stopping game'
            break
        }
    }
    Start-Sleep -Milliseconds 1500
}

Start-Sleep -Seconds 2   # let the last flush land
if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) }
Get-Process CoDSP -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not (Test-Path $log)) { Write-Host 'NO LOG PRODUCED'; exit 2 }
Write-Host "verdict reached: $verdict"
Write-Host "log: $log ($((Get-Item $log).Length) bytes)"
