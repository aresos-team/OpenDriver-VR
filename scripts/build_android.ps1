param(
    [string]$AndroidSdk = "",
    [string]$GradleCommand = "gradle",
    [string]$BuildTask = "assembleDebug"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$androidDir = Join-Path $projectRoot "android"

if (!(Test-Path $androidDir)) {
    throw "Android project not found at $androidDir"
}

if ($AndroidSdk) {
    $env:ANDROID_HOME = $AndroidSdk
    $env:ANDROID_SDK_ROOT = $AndroidSdk
}

Write-Host "Android project: $androidDir"
if ($env:ANDROID_SDK_ROOT) {
    Write-Host "Android SDK:     $env:ANDROID_SDK_ROOT"
} else {
    Write-Host "Android SDK:     not set in environment"
}
Write-Host "Gradle command:  $GradleCommand"
Write-Host "Build task:      $BuildTask"

Push-Location $androidDir
try {
    & $GradleCommand $BuildTask
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle build failed."
    }
} finally {
    Pop-Location
}
