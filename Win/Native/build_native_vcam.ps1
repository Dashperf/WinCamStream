$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repoRoot "Native\WcsNativeVCam"
$buildDir = Join-Path $srcDir "build"
$runtimeDir = Join-Path $repoRoot "Native\Runtime"

Write-Host "Configuring native VCam bridge..."
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

Write-Host "Building native VCam bridge (Release)..."
cmake --build $buildDir --config Release --target wcs_native_vcam
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed."
}

$exeCandidates = @(
    (Join-Path $buildDir "Release\wcs_native_vcam.exe"),
    (Join-Path $buildDir "wcs_native_vcam.exe")
)
$exePath = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exePath) {
    throw "Build succeeded but executable not found in expected output paths."
}

New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null
$dest = Join-Path $runtimeDir "wcs_native_vcam.exe"
try {
    Copy-Item -Path $exePath -Destination $dest -Force
} catch {
    $alt = "$dest.new"
    Copy-Item -Path $exePath -Destination $alt -Force
    Write-Warning "Target in use, wrote updated binary to $alt"
}

Write-Host "Native VCam bridge ready: $(Join-Path $runtimeDir 'wcs_native_vcam.exe')"
