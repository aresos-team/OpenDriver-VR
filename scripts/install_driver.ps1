param(
    [string]$SteamVRPath = "",
    [switch]$BuildRelease
)

$ErrorActionPreference = "Stop"

function Resolve-SteamVRPath {
    param([string]$OverridePath)

    if ($OverridePath -and (Test-Path $OverridePath)) {
        return (Resolve-Path $OverridePath).Path
    }

    $candidates = @(
        "C:\Program Files (x86)\Steam\steamapps\common\SteamVR",
        "C:\Program Files\Steam\steamapps\common\SteamVR",
        (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\SteamVR"),
        (Join-Path $env:ProgramFiles "Steam\steamapps\common\SteamVR")
    ) | Where-Object { $_ -and (Test-Path $_) }

    if ($candidates.Count -gt 0) {
        return (Resolve-Path $candidates[0]).Path
    }

    throw "SteamVR installation not found. Pass -SteamVRPath explicitly."
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$driverDir = Join-Path $projectRoot "driver"
$driverDll = Join-Path $driverDir "bin\win64\driver_opendriver.dll"
$runnerExe = Join-Path $driverDir "bin\win64\opendriver_runner.exe"
$pluginBuildDir = Join-Path $projectRoot "build\plugins\hmd_emulator"
$pluginDll = Join-Path $pluginBuildDir "hmd_emulator_plugin.dll"
$pluginManifest = Join-Path $pluginBuildDir "plugin.json"

# Android HMD (hmdek) plugin
$hmdekSourceDir = Join-Path $projectRoot "plugins\hmdek"
$hmdekBuildDir = Join-Path $hmdekSourceDir "build"
if(Test-Path (Join-Path $hmdekBuildDir "Release")) { $hmdekBuildDir = Join-Path $hmdekBuildDir "Release" }
$hmdekDll = Join-Path $hmdekBuildDir "android_hmd_plugin.dll"
$hmdekManifest = Join-Path $hmdekSourceDir "plugin.json"

$configDir = Join-Path $env:APPDATA "opendriver"
$pluginInstallDir = Join-Path $configDir "plugins\hmd_emulator"
$hmdekInstallDir = Join-Path $configDir "plugins\hmdek"

if ($BuildRelease) {
    Write-Host "Building Release configuration..."
    & cmake --build (Join-Path $projectRoot "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
}

if (!(Test-Path $driverDll)) {
    throw "Driver DLL not found: $driverDll"
}

if (!(Test-Path $runnerExe)) {
    throw "Runner EXE not found: $runnerExe"
}

if (!(Test-Path $pluginDll)) {
    throw "HMD emulator plugin DLL not found: $pluginDll"
}

if (!(Test-Path $pluginManifest)) {
    throw "HMD emulator plugin manifest not found: $pluginManifest"
}

if (!(Test-Path $hmdekDll)) {
    Write-Warning "Android HMD plugin DLL not found at $hmdekDll - skipping. Build it manually in plugins\hmdek."
}
if (!(Test-Path $hmdekManifest)) {
    throw "Android HMD plugin manifest not found: $hmdekManifest"
}

$steamVrResolved = Resolve-SteamVRPath -OverridePath $SteamVRPath
$vrPathReg = Join-Path $steamVrResolved "bin\win64\vrpathreg.exe"

if (!(Test-Path $vrPathReg)) {
    throw "vrpathreg.exe not found: $vrPathReg"
}

Write-Host "SteamVR path: $steamVrResolved"
Write-Host "Driver path:  $driverDir"
Write-Host "Config path:  $configDir"

New-Item -ItemType Directory -Force -Path $configDir | Out-Null
New-Item -ItemType Directory -Force -Path $hmdekInstallDir | Out-Null

# Android HMD (hmdek) plugin installation
if (Test-Path $hmdekDll) {
    Copy-Item $hmdekDll -Destination (Join-Path $hmdekInstallDir "android_hmd_plugin.dll") -Force
}
Copy-Item $hmdekManifest -Destination (Join-Path $hmdekInstallDir "plugin.json") -Force

Write-Host "Registering driver with SteamVR..."
& $vrPathReg removedriver $driverDir 2>$null | Out-Null
& $vrPathReg adddriver $driverDir
if ($LASTEXITCODE -ne 0) {
    throw "vrpathreg adddriver failed."
}

Write-Host ""
Write-Host "Installation complete."
Write-Host "Driver registered: $driverDir"
Write-Host "Plugin deployed:    $pluginInstallDir"
Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Restart SteamVR."
Write-Host "2. Start opendriver_runner.exe if SteamVR does not launch it automatically."
Write-Host "3. Verify that the emulated HMD appears in SteamVR and in the OpenDriver dashboard."
