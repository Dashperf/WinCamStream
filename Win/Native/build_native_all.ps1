$ErrorActionPreference = "Stop"

$winRoot = Split-Path -Parent $PSScriptRoot

Write-Host "Building native VCam bridge..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_vcam.ps1")

Write-Host "Building native control client..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_client.ps1")

Write-Host "Building native UI..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_ui.ps1")

$runtimeDir = Join-Path $winRoot "Native\Runtime"
New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null

$copyList = @(
    "Iproxy\iproxy.exe",
    "Iproxy\inetcat.exe",
    "Iproxy\libatomic-1.dll",
    "Iproxy\libgcc_s_seh-1.dll",
    "Iproxy\libgomp-1.dll",
    "Iproxy\libimobiledevice-glue-1.0.dll",
    "Iproxy\libplist++-2.0.dll",
    "Iproxy\libplist-2.0.dll",
    "Iproxy\libquadmath-0.dll",
    "Iproxy\libstdc++-6.dll",
    "Iproxy\libusbmuxd-2.0.dll",
    "Iproxy\libwinpthread-1.dll",
    "Iproxy\plistutil.exe",
    "ffmpeg-master-latest-win64-gpl-shared\bin\ffplay.exe",
    "ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avcodec-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avdevice-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avfilter-11.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avformat-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avutil-60.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\swscale-9.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\swresample-6.dll"
)

foreach ($rel in $copyList) {
    $src = Join-Path $winRoot $rel
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination (Join-Path $runtimeDir (Split-Path $src -Leaf)) -Force
    }
}

$unityInstallDir = Join-Path $runtimeDir "UnityCapture"
New-Item -ItemType Directory -Path $unityInstallDir -Force | Out-Null
Copy-Item -Path (Join-Path $winRoot "VCam\UnityCapture\Install\*") -Destination $unityInstallDir -Force

Write-Host "Native runtime package ready: $runtimeDir"
