param(
    [switch]$FetchAppleUsb
)

$ErrorActionPreference = "Stop"

$winRoot = Split-Path -Parent $PSScriptRoot

Write-Host "Building native VCam bridge..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_vcam.ps1")
if ($LASTEXITCODE -ne 0) { throw "build_native_vcam.ps1 failed." }

Write-Host "Building native control client..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_client.ps1")
if ($LASTEXITCODE -ne 0) { throw "build_native_client.ps1 failed." }

Write-Host "Building native UI..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_ui.ps1")
if ($LASTEXITCODE -ne 0) { throw "build_native_ui.ps1 failed." }

Write-Host "Building native WinUI..."
if ($FetchAppleUsb) {
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_winui.ps1") -FetchAppleUsb
} else {
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_winui.ps1")
}
if ($LASTEXITCODE -ne 0) { throw "build_native_winui.ps1 failed." }

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
        $dest = Join-Path $runtimeDir (Split-Path $src -Leaf)
        try {
            Copy-Item -Path $src -Destination $dest -Force
        } catch {
            Write-Warning "Skip copy (in use): $dest"
        }
    }
}

$depsSource = Join-Path $winRoot "Deps"
$depsTarget = Join-Path $runtimeDir "Deps"
if (Test-Path $depsSource) {
    New-Item -ItemType Directory -Path $depsTarget -Force | Out-Null
    Get-ChildItem -Path $depsSource -File | Where-Object {
        $_.Name -match 'Apple.*(Mobile|Device).*(Support|Driver).*\.(msi|exe)$'
    } | ForEach-Object {
        $dest = Join-Path $depsTarget $_.Name
        try {
            Copy-Item -Path $_.FullName -Destination $dest -Force
            Write-Host "Embedded Apple dependency copied: $($_.Name)"
        } catch {
            Write-Warning "Skip copy (in use): $dest"
        }
    }
}

$unityInstallDir = Join-Path $runtimeDir "UnityCapture"
New-Item -ItemType Directory -Path $unityInstallDir -Force | Out-Null
Get-ChildItem -Path (Join-Path $winRoot "VCam\UnityCapture\Install") -File | ForEach-Object {
    $dest = Join-Path $unityInstallDir $_.Name
    try {
        Copy-Item -Path $_.FullName -Destination $dest -Force
    } catch {
        Write-Warning "Skip copy (in use): $dest"
    }
}

Write-Host "Native runtime package ready: $runtimeDir"
