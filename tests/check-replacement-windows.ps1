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
$env:STAGEKIT_TEST_REPORT = $report
$bin = Join-Path $obsRoot 'bin/64bit'
$process = Start-Process -FilePath (Join-Path $bin 'obs64.exe') -WorkingDirectory $bin -PassThru -WindowStyle Hidden -ArgumentList @('--portable', '--multi', '--disable-shutdown-check', '--disable-missing-files-check', '--disable-updater', '--verbose')
try {
    if (!$process.WaitForExit(90000)) { throw 'Portable OBS did not exit within 90 seconds.' }
    if ($process.ExitCode -ne 0) { throw "Portable OBS exited with code $($process.ExitCode)." }
    if (!(Test-Path -LiteralPath $report)) { throw 'The OBS frontend probe did not run.' }
    $result = (Get-Content -Raw -LiteralPath $report).Trim()
    if ($result -ne 'PASS') { throw $result }
    Write-Output 'PASS: source and scene filters, six Tools entries, catalog search and clean Windows OBS shutdown.'
} catch {
    $logs = Get-ChildItem -LiteralPath (Join-Path $obsRoot 'config/obs-studio/logs') -Filter '*.txt' -ErrorAction SilentlyContinue
    if ($logs) { Get-Content -LiteralPath ($logs | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName -Tail 160 }
    if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
    throw
} finally {
    if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
}
