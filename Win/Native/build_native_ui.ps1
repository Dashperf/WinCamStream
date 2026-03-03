$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repoRoot "Native\WcsNativeUi"
$buildDir = Join-Path $srcDir "build"
$runtimeDir = Join-Path $repoRoot "Native\Runtime"

Write-Host "Configuring native UI client..."
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

$configured = $false
$generators = @(
    "Visual Studio 18 2026",
    "Visual Studio 17 2022",
    "Visual Studio 16 2019"
)
foreach ($gen in $generators) {
    Write-Host "Trying generator: $gen"
    cmake -S $srcDir -B $buildDir -G $gen -A x64
    if ($LASTEXITCODE -eq 0) {
        $configured = $true
        break
    }
    Write-Host "Generator failed: $gen"
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir
    }
}

if (-not $configured) {
    throw "Failed to configure CMake with a Visual Studio generator."
}

Write-Host "Building native UI client (Release)..."
cmake --build $buildDir --config Release --target wcs_native_ui
if ($LASTEXITCODE -ne 0) {
    throw "Native UI build failed."
}

$exeCandidates = @(
    (Join-Path $buildDir "Release\wcs_native_ui.exe"),
    (Join-Path $buildDir "wcs_native_ui.exe")
)
$exePath = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exePath) {
    throw "Build succeeded but executable not found in expected output paths."
}

New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null
Copy-Item -Path $exePath -Destination (Join-Path $runtimeDir "wcs_native_ui.exe") -Force

Write-Host "Native UI ready: $(Join-Path $runtimeDir 'wcs_native_ui.exe')"
