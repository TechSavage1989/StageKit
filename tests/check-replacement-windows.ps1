# Exercise the installed plugin in a disposable portable OBS 32.2.2 profile.
param([Parameter(Mandatory)][string] $PackageDirectory, [Parameter(Mandatory)][string] $ProbePath)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$probe = (Resolve-Path -LiteralPath $ProbePath).Path
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('stagekit-obs-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $scratch
$zip = Join-Path $scratch 'obs.zip'
$obsRoot = Join-Path $scratch 'portable'
$url = 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Windows-x64.zip'
Invoke-WebRequest -Uri $url -OutFile $zip
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne '4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1') {
    throw 'The portable OBS archive did not match the published SHA-256.'
}
Expand-Archive -LiteralPath $zip -DestinationPath $obsRoot
$pluginDir = Join-Path $obsRoot 'obs-plugins/64bit'
$dataDir = Join-Path $obsRoot 'data/obs-plugins/stagekit'
Copy-Item -LiteralPath (Join-Path $package 'bin/64bit/stagekit.dll') -Destination $pluginDir
Copy-Item -LiteralPath (Join-Path $package 'data') -Destination $dataDir -Recurse
Copy-Item -LiteralPath $probe -Destination (Join-Path $pluginDir 'stagekit-replacement-probe.dll')
$null = New-Item -ItemType Directory -Path (Join-Path $obsRoot 'data/obs-plugins/stagekit-replacement-probe')
$report = Join-Path $scratch 'result.txt'
& python (Join-Path $PSScriptRoot 'replacement-images.py') $scratch
if ($LASTEXITCODE -ne 0) { throw 'Could not generate the render fixture.' }
$env:STAGEKIT_TEST_ART = Join-Path $scratch 'art.png'
$env:STAGEKIT_TEST_REPORT = $report
$portCatalog = Get-Content -LiteralPath (Join-Path $dataDir 'ports.json') -Raw | ConvertFrom-Json
$keys = @($portCatalog.PSObject.Properties.Name | Where-Object { $_ -cne 'native' })
$moduleLabels = [ordered]@{ stagekit = 'StageKit' }
foreach ($line in Get-Content -LiteralPath (Join-Path $dataDir 'locale/en-US.ini')) {
    if ($line -cmatch '^Ports\.Module\.([a-z0-9]+)="(StageKit - [^"]+)"$') {
        $moduleLabels.Add(('stagekit-port-' + $Matches[1]), $Matches[2])
    }
}
foreach ($key in $keys) {
    if (!$moduleLabels.Contains("stagekit-port-$key")) { throw "Missing StageKit label for $key." }
}
if ($moduleLabels.Count -ne ($keys.Count + 1) -or @($moduleLabels.Values | Sort-Object -Unique).Count -ne $moduleLabels.Count) {
    throw 'Plugin Manager labels must be unique and cover exactly the packaged modules.'
}
$env:STAGEKIT_TEST_MODULE_NAMES = $moduleLabels.Values -join "`n"

# Exercise an upgrade with the display names saved by the previous release.
$managerDir = Join-Path $obsRoot 'config/obs-studio/plugin_manager'
$null = New-Item -ItemType Directory -Path $managerDir -Force
$moduleFile = Join-Path $managerDir 'modules.json'
$oldNames = [ordered]@{ stagekit = ''; 'stagekit-port-move' = 'Move Transition'; 'stagekit-port-composite' = ''; 'stagekit-port-ndi' = 'DistroAV' }
$oldModules = foreach ($name in $oldNames.Keys) {
    [ordered]@{ module_name = $name; display_name = $oldNames[$name]; id = ''; version = ''; enabled = $true
        sources = @(if ($name -ceq 'stagekit-port-ndi') { 'stagekit_port_ndi_source' }); outputs = @(); encoders = @(); services = @() }
}
$oldModules | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $moduleFile -Encoding utf8
$env:STAGEKIT_TEST_NDI = 'upgrade'
$bin = Join-Path $obsRoot 'bin/64bit'
$process = Start-Process -FilePath (Join-Path $bin 'obs64.exe') -WorkingDirectory $bin -PassThru -WindowStyle Hidden -ArgumentList @('--portable', '--multi', '--disable-shutdown-check', '--disable-missing-files-check', '--disable-updater', '--verbose')
try {
    if (!$process.WaitForExit(90000)) { throw 'Portable OBS did not exit within 90 seconds.' }
    if ($process.ExitCode -ne 0) { throw "Portable OBS exited with code $($process.ExitCode)." }
    if (!(Test-Path -LiteralPath $report)) { throw 'The OBS frontend probe did not run.' }
    $result = (Get-Content -Raw -LiteralPath $report).Trim()
    if ($result -ne 'PASS') { throw $result }
    $modules = Get-Content -LiteralPath $moduleFile -Raw | ConvertFrom-Json
    foreach ($name in $moduleLabels.Keys) {
        $entries = @($modules | Where-Object { $_.module_name -ceq $name })
        if ($entries.Count -ne 1 -or $entries[0].display_name -cne $moduleLabels[$name] -or !$entries[0].enabled) {
            throw "OBS did not register the expected enabled Plugin Manager entry for $name."
        }
    }
    Write-Output "PASS: OBS Plugin Manager shows all $($moduleLabels.Count) StageKit names once and refreshes the previous release's labels."
    $expectedOwners = [ordered]@{
        stagekit_ambient_background = 'stagekit'
        stagekit_cropped_view = 'stagekit'
        stagekit_start_image = 'stagekit'
        stagekit_countdown = 'stagekit'
        stagekit_scene_fit = 'stagekit'
        stagekit_morph = 'stagekit'
        stagekit_gaussian_blur = 'stagekit'
        stagekit_port_obs_composite_blur = 'stagekit-port-composite'
    }
    foreach ($sourceId in $expectedOwners.Keys) {
        $owners = @($modules | Where-Object { $_.sources -ccontains $sourceId })
        $expected = $expectedOwners[$sourceId]
        if ($owners.Count -ne 1 -or !$owners[0].enabled -or $owners[0].module_name -cne $expected) {
            throw "OBS did not associate $sourceId with the enabled $expected module."
        }
    }
    Write-Output 'PASS: OBS recognizes the native StageKit sources, filters and transition and the bundled Composite Blur filter.'
    $testLog = Get-ChildItem -LiteralPath (Join-Path $obsRoot 'config/obs-studio/logs') -Filter '*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Select-String -LiteralPath $testLog.FullName -SimpleMatch -Pattern '[camera transition test]', '[Quick Access test]', '[Studio transition test]', '[capture settings test]', 'StageKit timer layout:', 'StageKit timer at ', 'StageKit timer dock:' | ForEach-Object { Write-Output $_.Line }
    Write-Output 'PASS: source and scene filters, six Tools entries, catalog search, three visible Composite Blur modes and clean Windows OBS shutdown.'
    $env:STAGEKIT_TEST_NDI = 'restart'
    $env:STAGEKIT_TEST_REPORT = Join-Path $scratch 'ndi-restart.txt'
    $second = Start-Process -FilePath (Join-Path $bin 'obs64.exe') -WorkingDirectory $bin -PassThru -WindowStyle Hidden -ArgumentList @('--portable', '--multi', '--disable-shutdown-check', '--disable-missing-files-check', '--disable-updater')
    try {
        if (!$second.WaitForExit(90000)) { throw 'Portable OBS did not finish the NDI restart check.' }
        if ($second.ExitCode -ne 0 -or !(Test-Path -LiteralPath $env:STAGEKIT_TEST_REPORT)) { throw 'The NDI restart check did not finish.' }
        $restartResult = (Get-Content -Raw -LiteralPath $env:STAGEKIT_TEST_REPORT).Trim()
        if ($restartResult -ne 'PASS') { throw $restartResult }
        Write-Output 'PASS: Quick Access restored Manual, Dynamic and Source Search docks with the saved manual order and dock options.'
        Write-Output 'PASS: StageKit retained prior NDI support on upgrade and skipped the NDI module after turning it off and restarting OBS.'
    } finally {
        if (!$second.HasExited) { $second.Kill(); $second.WaitForExit() }
    }
} catch {
    $logs = Get-ChildItem -LiteralPath (Join-Path $obsRoot 'config/obs-studio/logs') -Filter '*.txt' -ErrorAction SilentlyContinue
    if ($logs) { Select-String -LiteralPath ($logs | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName -SimpleMatch -Pattern 'StageKit timer' | ForEach-Object { Write-Output $_.Line } }
    if ($logs) { Get-Content -LiteralPath ($logs | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName -Tail 160 }
    if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
    throw
} finally {
    if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
}
