param(
    [Parameter(Position = 0)]
    [string] $Target
)

$ErrorActionPreference = 'Stop'
$adbName = if ($env:ADB) { $env:ADB } else { 'adb' }
$adbCommand = Get-Command $adbName -ErrorAction SilentlyContinue
if (-not $adbCommand) {
    [Console]::Error.WriteLine('Android platform-tools (adb.exe) is required on this computer.')
    exit 2
}
$adb = $adbCommand.Source

if ([string]::IsNullOrWhiteSpace($Target)) {
    $deviceLines = @(& $adb devices 2>&1)
    if ($LASTEXITCODE -ne 0) { [Console]::Error.WriteLine('Could not query ADB devices.'); exit 1 }
    $connected = @(
        foreach ($line in $deviceLines) {
            if ([string]$line -match '^\s*(\S+)\s+device\s*$') { $Matches[1] }
        }
    )
    if ($connected.Count -ne 1) {
        [Console]::Error.WriteLine("Expected exactly one connected ADB device; found $($connected.Count). Pass panel-ip[:adb-port] or an ADB serial.")
        exit 2
    }
    $Target = $connected[0]
}
elseif ($Target -match '^(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?$') {
    if ($Target -notmatch ':') { $Target = "$Target`:1379" }
    & $adb connect $Target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$state = & $adb -s $Target get-state 2>$null
if ($LASTEXITCODE -ne 0 -or $state -ne 'device') {
    [Console]::Error.WriteLine("ADB target is unavailable: $Target. Enable/authorize ADB and check network reachability.")
    exit 1
}
$rootIdentity = & $adb -s $Target shell su 0 id 2>&1
if ($LASTEXITCODE -ne 0 -or (($rootIdentity -join "`n") -notmatch 'uid=0')) {
    [Console]::Error.WriteLine("ADB root access failed: $($rootIdentity -join ' ')")
    exit 1
}

$remoteScript = '/data/local/tmp/ihg2-arm-preflight.sh'
$deviceScript = Join-Path $PSScriptRoot 'preflight-device.sh'
if (-not (Test-Path -LiteralPath $deviceScript -PathType Leaf)) {
    [Console]::Error.WriteLine('Preflight device script is missing from this release bundle.')
    exit 2
}
& $adb -s $Target push $deviceScript $remoteScript
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $adb -s $Target shell su 0 sh $remoteScript
exit $LASTEXITCODE
