# Play every test replay headless and report PASS/FAIL (Windows), the counterpart of
# run-replay-tests-macos.sh. The replays are the macOS recordings, so a pass here means
# the Windows build simulates exactly like the macOS one (cross-platform determinism).
#
#   powershell -ExecutionPolicy Bypass -File scripts\qa\replays\run-replay-tests-windows.ps1 `
#       -Game <GeneralsXZH.exe> [-ReplayDir <dir>] [-ZeroHourData <dir>] [-GeneralsData <dir>]
#
# Defaults: replays are macos_*.rep from GeneralsReplays\GeneralsXZH\Replays and
# Tests\replays\macos (a later dir replaces a replay with the same name); with -ReplayDir
# only that dir is used. Old 4-byte wide char replays are converted with
# convert_wchar32_replay.py when Python is found. Maps come from GeneralsReplays\GeneralsXZH\Maps.
# Game data: -ZeroHourData / -GeneralsData, else CNC_GENERALS_ZH_PATH / CNC_GENERALS_PATH,
# else the InstallPath registry values.
#
# The run uses a throwaway user data folder (GENERALSX_USER_DATA_DIR under -WorkDir, default
# %TEMP%), so the real Documents profile, maps and replays are not touched. The game runs
# with -headless: no window. Logs: logs\replay-tests\ (<name>.log, and <name>.crc with the
# compared CRCs as "frame crc" lines, for diffing against another platform). -KeepDebug <dir>
# keeps the game's Debug folder (deep CRC dumps written on a mismatch).
#
# A replay passes only if the game exits 0 AND compared at least one CRC (REPLAY_CRC_COMPARE
# lines) AND none of them mismatched. Exit code: number of failed replays (0 = all passed).
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$ReplayDir = "",
    [string]$ZeroHourData = "",
    [string]$GeneralsData = "",
    [string]$WorkDir = $env:TEMP,
    [string]$ExtraArgs = "",
    [int]$TimeoutSeconds = 900,
    [string]$KeepDebug = ""
)
$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Game = (Resolve-Path $Game).Path

function Get-InstallPath([string]$key) {
    foreach ($view in @('Registry32', 'Registry64')) {
        foreach ($hive in @('CurrentUser', 'LocalMachine')) {
            try {
                $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
                $k = $base.OpenSubKey($key)
                if ($k) { $v = $k.GetValue('InstallPath'); if ($v) { return $v.TrimEnd('\') } }
            } catch { }
        }
    }
    return ""
}
if (-not $ZeroHourData) { $ZeroHourData = $env:CNC_GENERALS_ZH_PATH }
if (-not $ZeroHourData) { $ZeroHourData = Get-InstallPath 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour' }
if (-not $GeneralsData) { $GeneralsData = $env:CNC_GENERALS_PATH }
if (-not $GeneralsData) { $GeneralsData = Get-InstallPath 'Software\Electronic Arts\EA Games\Generals' }
if (-not $ZeroHourData -or -not (Test-Path $ZeroHourData)) { Write-Error "Zero Hour data not found"; exit 100 }
if (-not $GeneralsData -or -not (Test-Path $GeneralsData)) { Write-Error "Generals data not found"; exit 100 }

if ($ReplayDir) { $replayDirs = @($ReplayDir) }
else { $replayDirs = @((Join-Path $projectRoot 'GeneralsReplays\GeneralsXZH\Replays'), (Join-Path $projectRoot 'Tests\replays\macos')) }
$replays = [ordered]@{}
foreach ($dir in $replayDirs) {
    if (-not (Test-Path $dir)) { continue }
    foreach ($rep in Get-ChildItem $dir -Filter 'macos_*.rep' | Sort-Object Name) { $replays[$rep.Name] = $rep.FullName }
}
if ($replays.Count -eq 0) { Write-Error "no macos_*.rep in $($replayDirs -join ', ')"; exit 100 }

$converter = Join-Path $projectRoot 'scripts\replays\convert_wchar32_replay.py'
$python = Get-Command python -ErrorAction SilentlyContinue
$logDir = Join-Path $projectRoot 'logs\replay-tests'
$userData = Join-Path $WorkDir ("replay-profile-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force (Join-Path $userData 'Maps'), (Join-Path $userData 'Replays'), $logDir | Out-Null
$mapsDir = Join-Path $projectRoot 'GeneralsReplays\GeneralsXZH\Maps'
if (Test-Path $mapsDir) { Copy-Item (Join-Path $mapsDir '*') (Join-Path $userData 'Maps') -Recurse -Force }

# The game makes the folder of the exe its working directory (unless -useCwd) and reads the .big
# archives and the loose files (e.g. Data\Scripts\SkirmishScripts.scb) from CNC_GENERALS_ZH_PATH,
# so the exe runs from where it was built. (-useCwd with the Zero Hour folder as the working
# directory crashes in audio init, presumably on the retail mss32.dll found there: not used.)
$gameDir = Split-Path $Game

$env:GENERALSX_USER_DATA_DIR = $userData
$env:CNC_GENERALS_ZH_PATH = $ZeroHourData
$env:CNC_GENERALS_PATH = $GeneralsData

$pass = 0; $fail = 0
try {
    foreach ($name in $replays.Keys) {
        $src = $replays[$name]
        $dst = Join-Path $userData "Replays\$name"
        # The converter refuses a replay that is already in the new format; then copy it as it is.
        $converted = $false
        if ($python) {
            # Windows PowerShell 5 turns a native command's stderr into an error under 'Stop'.
            $ErrorActionPreference = 'Continue'
            & $python.Source -I $converter $src $dst 2>&1 | Out-Null
            $converted = ($LASTEXITCODE -eq 0)
            $ErrorActionPreference = 'Stop'
        }
        if (-not $converted) { Copy-Item $src $dst -Force }

        $base = [IO.Path]::GetFileNameWithoutExtension($name)
        $log = Join-Path $logDir "$base.log"
        $out = Join-Path $logDir "$base.stdout.tmp"
        $err = Join-Path $logDir "$base.stderr.tmp"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath $Game -ArgumentList "-headless $ExtraArgs -replay `"$dst`"" `
            -WorkingDirectory $gameDir -RedirectStandardOutput $out -RedirectStandardError $err `
            -PassThru -WindowStyle Hidden
        $null = $p.Handle  # without a cached handle, ExitCode stays empty after the process exits
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $p.Id -Force
            $code = 'timeout'
        } else {
            $p.WaitForExit(); $code = $p.ExitCode
        }
        $secs = [int]$sw.Elapsed.TotalSeconds
        Get-Content $err, $out -ErrorAction SilentlyContinue | Set-Content $log
        Remove-Item $out, $err -ErrorAction SilentlyContinue

        $lines = @(Select-String -LiteralPath $log -Pattern 'REPLAY_CRC_COMPARE frame=(\d+) inGame=0x([0-9A-F]+)')
        $lines | ForEach-Object { "$($_.Matches[0].Groups[1].Value) $($_.Matches[0].Groups[2].Value)" } |
            Set-Content (Join-Path $logDir "$base.crc")
        $compares = $lines.Count
        $mismatches = @($lines | Where-Object { $_.Line -match ' MISMATCH' }).Count
        if ($code -eq 0 -and $compares -eq 0) {
            # Exit 0 without a single compared CRC (map did not load, CRC sent by another slot,
            # replay too short) means nothing, so it does not count as a pass.
            $fail++
            "FAIL  $name  ($($secs)s)  CRC was never compared  log: $log"
        } elseif ($code -eq 0 -and $mismatches -eq 0) {
            $pass++
            "PASS  $name  ($($secs)s)  $compares CRC compares, all equal"
        } else {
            $fail++
            $detail = (Select-String -LiteralPath $log -Pattern 'CRC Mismatch|Cannot open replay|Game Time' |
                ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique | Select-Object -Last 2) -join ' '
            "FAIL  $name  exit $code ($($secs)s)  $compares compares, $mismatches mismatched  $detail  log: $log"
        }
    }
} finally {
    # On a mismatch the game writes deep CRC dumps (the serialized state of the last CRC frames) to Debug.
    if ($KeepDebug -and (Test-Path (Join-Path $userData 'Debug'))) {
        New-Item -ItemType Directory -Force $KeepDebug | Out-Null
        Copy-Item (Join-Path $userData 'Debug\*') $KeepDebug -Recurse -Force
    }
    Remove-Item $userData -Recurse -Force -ErrorAction SilentlyContinue
}
"Results: $pass passed, $fail failed"
exit $fail
