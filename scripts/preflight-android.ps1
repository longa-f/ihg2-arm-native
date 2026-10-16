param(
    [Parameter(Position = 0)]
    [string] $Target
)

$ErrorActionPreference = 'Stop'

$script:TranscriptStarted = $false

function Stop-PreflightTranscript([int] $ExitCode) {
    if (-not $script:TranscriptStarted) { return }
    Write-Host "Preflight finished at $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK') with exit code $ExitCode"
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

function Fail([string] $Message, [int] $ExitCode = 1) {
    Write-Error -Message $Message -ErrorAction Continue
    Stop-PreflightTranscript $ExitCode
    exit $ExitCode
}

try {
    $logDirectory = Join-Path $PSScriptRoot 'logs'
    if (Test-Path -LiteralPath $logDirectory) {
        $logDirectoryItem = Get-Item -LiteralPath $logDirectory -Force
        if (-not $logDirectoryItem.PSIsContainer -or
            ($logDirectoryItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Unsafe preflight log directory: $logDirectory"
        }
    }
    else {
        New-Item -ItemType Directory -Path $logDirectory | Out-Null
    }
    $logTimestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $script:LogPath = Join-Path $logDirectory "preflight-$logTimestamp-$PID.log"
    Start-Transcript -Path $script:LogPath -NoClobber | Out-Null
    $script:TranscriptStarted = $true
    Write-Host "Preflight log: $script:LogPath"
}
catch {
    [Console]::Error.WriteLine("Could not start preflight log: $($_.Exception.Message)")
    exit 2
}

$adbName = if ($env:ADB) { $env:ADB } else { 'adb' }
$adbCommand = Get-Command $adbName -ErrorAction SilentlyContinue
if (-not $adbCommand) {
    Fail 'Android platform-tools (adb.exe) is required on this computer.' 2
}
$adb = $adbCommand.Source

if ([string]::IsNullOrWhiteSpace($Target)) {
    $deviceLines = @(& $adb devices 2>&1)
    if ($LASTEXITCODE -ne 0) { Fail 'Could not query ADB devices.' }
    $connected = @(
        foreach ($line in $deviceLines) {
            if ([string]$line -match '^\s*(\S+)\s+device\s*$') { $Matches[1] }
        }
    )
    if ($connected.Count -ne 1) {
        Fail "Expected exactly one connected ADB device; found $($connected.Count). Pass panel-ip[:adb-port] or an ADB serial." 2
    }
    $Target = $connected[0]
}
elseif ($Target -match '^(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?$') {
    if ($Target -notmatch ':') { $Target = "$Target`:1379" }
    & $adb connect $Target
    if ($LASTEXITCODE -ne 0) { Fail "ADB could not connect to $Target." $LASTEXITCODE }
}

$stateOutput = & $adb -s $Target get-state 2>&1
$stateExitCode = $LASTEXITCODE
$state = ($stateOutput -join "`n").Trim()
if ($stateExitCode -ne 0 -or $state -ne 'device') {
    if ($state) { Write-Host "ADB get-state output: $state" }
    Fail "ADB target is unavailable: $Target. Enable/authorize ADB and check network reachability."
}
$rootIdentity = & $adb -s $Target shell su 0 id 2>&1
if ($LASTEXITCODE -ne 0 -or (($rootIdentity -join "`n") -notmatch 'uid=0')) {
    Fail "ADB root access failed: $($rootIdentity -join ' ')"
}
Write-Host "ADB target: $Target"

$remoteScript = '/data/local/tmp/ihg2-arm-preflight.sh'
$deviceScript = Join-Path $PSScriptRoot 'preflight-device.sh'
if (-not (Test-Path -LiteralPath $deviceScript -PathType Leaf)) {
    Fail 'Preflight device script is missing from this release bundle.' 2
}
& $adb -s $Target push $deviceScript $remoteScript
if ($LASTEXITCODE -ne 0) {
    $result = $LASTEXITCODE
    Stop-PreflightTranscript $result
    exit $result
}
& $adb -s $Target shell su 0 sh $remoteScript
$result = $LASTEXITCODE
Stop-PreflightTranscript $result
exit $result
