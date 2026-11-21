param(
    # Where the plugin log of the run is copied.
    [Parameter(Mandatory)][string]$Output,
    # Counted minutes ("VM catch-up: frames=" lines) to wait for after the hooks are installed.
    [ValidateRange(1, 10)][int]$Minutes = 2
)
# One launch of the release DLL in the MO2 test profile APB-CatchUp-Release, which loads Save93 through
# APBTestSaveLoader at a 30 FPS limit (SSEDisplayTweaks_Custom.ini of the mod "APB CatchUp Release Test").
# Succeeds when PapyrusCatchUp.log shows the hooks installed, the wanted number of minute lines, no
# warning or error, and no guard violation. MO2 must be closed; its selected profile is put back.
$ErrorActionPreference = 'Stop'
$mo2Dir = 'G:\TAKEALOOK'
$profile = 'APB-CatchUp-Release'
$logs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SKSE'
$log = Join-Path $logs 'PapyrusCatchUp.log'
$iniPath = Join-Path $mo2Dir 'ModOrganizer.ini'

if (Get-Process SkyrimSE, ModOrganizer -ErrorAction SilentlyContinue) { throw 'Close Skyrim and MO2 first.' }
if (Test-Path -LiteralPath $Output) { throw "Existing output: $Output" }
$save = (Get-ChildItem -LiteralPath (Join-Path $mo2Dir "profiles\$profile\saves") -Filter 'Save93_*.ess' -File | Sort-Object Name | Select-Object -First 1).BaseName
if (-not $save) { throw 'Save93 is missing from the test profile.' }
$iniBefore = [System.IO.File]::ReadAllBytes($iniPath)

$started = Get-Date
$launcher = $null
$game = $null
try {
    # Started through WMI so that MO2 runs outside the job object of the calling shell: inside one
    # (Claude Code, Antigravity) MO2 fails to start skse64_loader.exe with ERROR_ACCESS_DENIED.
    # A WMI process does not inherit this shell's environment, so cmd sets the save for the loader.
    $command = 'cmd.exe /c set "APB_TEST_SAVE={0}" && "{1}" -p "{2}" "moshortcut://:SKSE"' -f $save, (Join-Path $mo2Dir 'ModOrganizer.exe'), $profile
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $command; CurrentDirectory = $mo2Dir }
    if ($created.ReturnValue -ne 0) { throw "Could not start MO2 through WMI (code $($created.ReturnValue))." }
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        $launcher = Get-Process ModOrganizer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt $started } | Select-Object -First 1
    } until ($launcher -or (Get-Date) -gt $deadline)
    if (-not $launcher) { throw 'MO2 did not start.' }
    $deadline = (Get-Date).AddMinutes(3)
    do {
        Start-Sleep -Milliseconds 750
        $game = Get-Process SkyrimSE -ErrorAction SilentlyContinue | Select-Object -First 1
        if ((Get-Date) -gt $deadline) { throw 'Skyrim did not start (see MO2 logs).' }
    } until ($game)
    "Skyrim $($game.Id) started"
    $deadline = (Get-Date).AddMinutes(4 + $Minutes)
    do {
        Start-Sleep -Seconds 3
        if ($game.HasExited) { throw "Skyrim exited early with code $($game.ExitCode)." }
        if ((Get-Date) -gt $deadline) { throw 'Timed out waiting for the minute lines.' }
        $item = Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
        $lines = if ($item -and $item.LastWriteTime -gt $started) { @(Get-Content -LiteralPath $log) } else { @() }
        $minuteLines = @($lines -match 'VM catch-up: frames=')
    } until ($minuteLines.Count -ge $Minutes + 1)
}
finally {
    if ($game -and -not $game.HasExited) {
        $game.Refresh()
        [void]$game.CloseMainWindow()
        if (-not $game.WaitForExit(25000)) { Stop-Process -Id $game.Id -ErrorAction SilentlyContinue; [void]$game.WaitForExit(5000) }
    }
    if ($launcher -and -not $launcher.WaitForExit(15000)) {
        [void]$launcher.CloseMainWindow()
        if (-not $launcher.WaitForExit(15000)) { Stop-Process -Id $launcher.Id -ErrorAction SilentlyContinue }
    }
    # MO2 is closed again here, so its whole settings file goes back to the state before the run
    # (the launch switches the selected profile; nothing else should have written it meanwhile).
    [System.IO.File]::WriteAllBytes($iniPath, $iniBefore)
    'ModOrganizer.ini restored'
    if ((Get-Item -LiteralPath $log -ErrorAction SilentlyContinue).LastWriteTime -gt $started) {
        Copy-Item -LiteralPath $log -Destination $Output
    }
}

$lines = @(Get-Content -LiteralPath $Output)
$problems = @($lines -match '\[(W|E|C)\]') + @($lines -match 'guard_violations=[1-9]') + @($lines -match 'paused for')
# The run must have done the thing it checks: extra cycles in at least one minute line.
$ran = @($lines -match 'extra_cycles=[1-9]').Count -gt 0
$lines -match 'Loaded PapyrusCatchUp|Installed VM catch-up|reads:|already hooked|VM catch-up: frames=' | ForEach-Object { $_.Substring(0, [Math]::Min(230, $_.Length)) }
if (-not ($lines -match 'Installed VM catch-up') -or -not $ran -or $problems.Count) {
    $problems
    throw 'Smoke run FAILED: hooks not installed, no extra cycle, or warnings, violations or overrun pauses in the log.'
}
'Smoke run passed'
