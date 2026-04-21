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

function Assert-FileExists {
    param(
        [string]$Path,
        [string]$Label
    )

    if (!(Test-Path $Path)) {
        throw "$Label not found: $Path"
    }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$driverDir = Join-Path $projectRoot "driver"
$driverManifest = Join-Path $driverDir "driver.vrdrivermanifest"
$driverDll = Join-Path $driverDir "bin\win64\driver_opendriver.dll"
$runnerExe = Join-Path $driverDir "bin\win64\opendriver_runner.exe"
$qtCoreDll = Join-Path $driverDir "bin\win64\Qt6Core.dll"
$qtGuiDll = Join-Path $driverDir "bin\win64\Qt6Gui.dll"
$qtWidgetsDll = Join-Path $driverDir "bin\win64\Qt6Widgets.dll"
$qtPlatformDll = Join-Path $driverDir "bin\win64\platforms\qwindows.dll"
$inputProfile = Join-Path $driverDir "resources\input\opendriver_hmd_profile.json"
$inputBindings = Join-Path $driverDir "resources\input\opendriver_hmd_vrcompositor_bindings.json"

if ($BuildRelease) {
    Write-Host "Building Release configuration..."
    & cmake --build (Join-Path $projectRoot "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
}

Assert-FileExists -Path $driverManifest -Label "Driver manifest"
Assert-FileExists -Path $driverDll -Label "Driver DLL"
Assert-FileExists -Path $runnerExe -Label "Runner EXE"
Assert-FileExists -Path $qtCoreDll -Label "Qt6Core.dll"
Assert-FileExists -Path $qtGuiDll -Label "Qt6Gui.dll"
Assert-FileExists -Path $qtWidgetsDll -Label "Qt6Widgets.dll"
Assert-FileExists -Path $qtPlatformDll -Label "Qt platform plugin"
Assert-FileExists -Path $inputProfile -Label "Input profile"
Assert-FileExists -Path $inputBindings -Label "Input bindings"

$steamVrResolved = Resolve-SteamVRPath -OverridePath $SteamVRPath
$vrPathReg = Join-Path $steamVrResolved "bin\win64\vrpathreg.exe"
Assert-FileExists -Path $vrPathReg -Label "vrpathreg.exe"

$configDir = Join-Path $env:APPDATA "opendriver"
$pluginsDir = Join-Path $configDir "plugins"

Write-Host "SteamVR path:      $steamVrResolved"
Write-Host "Driver path:       $driverDir"
Write-Host "Config path:       $configDir"
Write-Host "Plugins directory: $pluginsDir"
Write-Host ""
Write-Host "Installing ONLY the native OpenDriver SteamVR driver."
Write-Host "No plugins will be copied or registered."

New-Item -ItemType Directory -Force -Path $configDir | Out-Null
New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null

Write-Host ""
Write-Host "Refreshing SteamVR driver registration..."
& $vrPathReg removedriver $driverDir 2>$null | Out-Null
& $vrPathReg adddriver $driverDir
if ($LASTEXITCODE -ne 0) {
    throw "vrpathreg adddriver failed."
}

Write-Host ""
Write-Host "Installation complete."
Write-Host "Registered driver: $driverDir"
Write-Host "Driver payload:    driver_opendriver.dll + SteamVR resources"
Write-Host "Plugin deployment: skipped"
Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Restart SteamVR."
Write-Host "2. Put plugins into $pluginsDir only if you want them later."
