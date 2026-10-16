param(
    [Parameter(Position = 0)]
    [string] $Target,
    [switch] $Doctor,
    [switch] $ConfigureLock,
    [string] $LockIp,
    [string] $LockMac
)

$ErrorActionPreference = 'Stop'

function Fail([string] $Message) {
    [Console]::Error.WriteLine($Message)
    exit 1
}

$adbName = if ($env:ADB) { $env:ADB } else { 'adb' }
$adbCommand = Get-Command $adbName -ErrorAction SilentlyContinue
if (-not $adbCommand) {
    Fail 'Android platform-tools (adb.exe) is required on this computer.'
}
$script:AdbPath = $adbCommand.Source

if ([string]::IsNullOrWhiteSpace($Target)) {
    $deviceLines = @(& $script:AdbPath devices 2>&1)
    if ($LASTEXITCODE -ne 0) { Fail 'Could not query ADB devices.' }
    $connected = @(
        foreach ($line in $deviceLines) {
            if ([string]$line -match '^\s*(\S+)\s+device\s*$') { $Matches[1] }
        }
    )
    if ($connected.Count -ne 1) {
        Fail "Expected exactly one connected ADB device; found $($connected.Count). Pass panel-ip[:adb-port] or an ADB serial."
    }
    $Target = $connected[0]
}
elseif ($Target -match '^(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?$') {
    if ($Target -notmatch ':') { $Target = "$Target`:1379" }
    & $script:AdbPath connect $Target
    if ($LASTEXITCODE -ne 0) { Fail "ADB could not connect to $Target." }
}

$state = & $script:AdbPath -s $Target get-state 2>$null
if ($LASTEXITCODE -ne 0 -or $state -ne 'device') {
    Fail "ADB target is unavailable: $Target. Enable/authorize ADB and check network reachability."
}
$rootIdentity = & $script:AdbPath -s $Target shell su 0 id 2>&1
if ($LASTEXITCODE -ne 0 -or (($rootIdentity -join "`n") -notmatch 'uid=0')) {
    Fail "ADB root access failed: $($rootIdentity -join ' ')"
}

if ($Doctor -and $ConfigureLock) { Fail 'Choose either -Doctor or -ConfigureLock.' }
if ($Doctor) {
    & $script:AdbPath -s $Target shell su 0 /data/local/ihg2-arm-native/ihg2-arm-bridge --doctor
    exit $LASTEXITCODE
}
if ($ConfigureLock) {
    if ([string]::IsNullOrWhiteSpace($LockIp) -or [string]::IsNullOrWhiteSpace($LockMac) -or
        $LockIp -notmatch '^(?:\d{1,3}\.){3}\d{1,3}$' -or
        $LockMac -notmatch '^(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$') {
        Fail 'Lock configuration expects -LockIp IPv4 -LockMac AA:BB:CC:DD:EE:FF.'
    }
    & $script:AdbPath -s $Target shell su 0 /data/local/ihg2-arm-native/ihg2-arm-bridge --configure-lock $LockIp $LockMac
    exit $LASTEXITCODE
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$bundleRoot = $PSScriptRoot
$binary = Join-Path $bundleRoot 'ihg2-arm-bridge'
$versionFile = Join-Path $bundleRoot 'VERSION'
$supervisor = Join-Path $bundleRoot 'supervisor.sh'
$bootHook = Join-Path $bundleRoot 'install-recovery.sh'
$deviceInstaller = Join-Path $bundleRoot 'install-device.sh'
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    $binary = Join-Path $repoRoot 'build/android-armv7/ihg2-arm-bridge'
    $versionFile = Join-Path $repoRoot 'VERSION'
    $supervisor = Join-Path $repoRoot 'scripts/supervisor.sh'
    $bootHook = Join-Path $repoRoot 'scripts/install-recovery.sh'
    $deviceInstaller = Join-Path $repoRoot 'scripts/install-device.sh'
}
foreach ($file in @($binary, $versionFile, $supervisor, $bootHook, $deviceInstaller)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Fail "Deployment bundle is incomplete or the ARM binary is missing: $file"
    }
}
$version = [System.IO.File]::ReadAllText($versionFile).Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    Fail 'VERSION must contain one SemVer MAJOR.MINOR.PATCH value.'
}
$binaryText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($binary))
if (-not $binaryText.Contains('IHG2_ARM_AUTO_ONLY_V1')) {
    Fail 'ARM bridge is stale or is not an automatic-only build. Rebuild before deployment.'
}
if (-not $binaryText.Contains("IHG2 ARM bridge version $version")) {
    Fail "Release VERSION and ARM binary do not match (VERSION=$version). Rebuild the release."
}

$remoteStage = '/data/local/tmp/ihg2-arm-deploy-v2'
Write-Host "Deploying IHG2 ARM bridge v$version"
& $script:AdbPath -s $Target shell "mkdir -p '$remoteStage' && chmod 0700 '$remoteStage'"
if ($LASTEXITCODE -ne 0) { Fail 'Could not create the temporary deployment directory on the panel.' }

function Push-DeploymentFile([string] $LocalPath, [string] $RemotePath) {
    & $script:AdbPath -s $Target push $LocalPath $RemotePath
    if ($LASTEXITCODE -ne 0) { Fail "Could not copy deployment file to the panel: $LocalPath" }
}

Push-DeploymentFile $binary "$remoteStage/ihg2-arm-bridge"
Push-DeploymentFile $versionFile "$remoteStage/VERSION"
Push-DeploymentFile $supervisor "$remoteStage/supervisor.sh"
Push-DeploymentFile $bootHook "$remoteStage/install-recovery.sh"
Push-DeploymentFile $deviceInstaller "$remoteStage/install-device.sh"
& $script:AdbPath -s $Target shell su 0 "$remoteStage/ihg2-arm-bridge" --check-panel-sdk
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $script:AdbPath -s $Target shell su 0 sh "$remoteStage/install-device.sh"
exit $LASTEXITCODE
