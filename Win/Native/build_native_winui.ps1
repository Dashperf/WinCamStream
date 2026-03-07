param(
    [switch]$FetchAppleUsb
)

$ErrorActionPreference = "Stop"

$projectDir = Join-Path $PSScriptRoot "WcsNativeWinUI"
$projectFile = Join-Path $projectDir "WcsNativeWinUI.vcxproj"

if (-not (Test-Path $projectFile)) {
    throw "Project file not found: $projectFile"
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found: $vswhere"
}

$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) {
    throw "Visual Studio installation not found."
}

$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    throw "MSBuild not found: $msbuild"
}

Write-Host "Restoring WinUI packages..."
& $msbuild $projectFile -t:Restore -p:RestorePackagesConfig=true -m
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed for WinUI project." }

Write-Host "Building WinUI client (Release x64)..."
& $msbuild $projectFile -p:Configuration=Release -p:Platform=x64 -m
if ($LASTEXITCODE -ne 0) { throw "WinUI build failed." }

$outDir = Join-Path $projectDir "x64\Release\WcsNativeWinUI"
if (-not (Test-Path $outDir)) {
    throw "WinUI output directory not found: $outDir"
}

$runtimeRoot = Join-Path $PSScriptRoot "Runtime"
$runtimeDir = Join-Path $runtimeRoot "WcsNativeWinUI"
New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null

Write-Host "Copying WinUI runtime payload..."
Get-ChildItem -Path $outDir | ForEach-Object {
    $dest = Join-Path $runtimeDir $_.Name
    try {
        if ($_.PSIsContainer) {
            if (Test-Path $dest) {
                Remove-Item -Path $dest -Recurse -Force
            }
            Copy-Item -Path $_.FullName -Destination $dest -Recurse -Force
        } else {
            Copy-Item -Path $_.FullName -Destination $dest -Force
        }
    } catch {
        Write-Warning "Skip copy (in use): $dest"
    }
}

$winRoot = Split-Path -Parent $PSScriptRoot

$embeddedTools = @(
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
    "ffmpeg-master-latest-win64-gpl-shared\bin\avcodec-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avdevice-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avfilter-11.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avformat-62.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\avutil-60.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\swresample-6.dll",
    "ffmpeg-master-latest-win64-gpl-shared\bin\swscale-9.dll"
)

Write-Host "Copying embedded iProxy/ffplay tools..."
foreach ($rel in $embeddedTools) {
    $src = Join-Path $winRoot $rel
    if (Test-Path $src) {
        $dest = Join-Path $runtimeRoot (Split-Path $src -Leaf)
        try {
            Copy-Item -Path $src -Destination $dest -Force
        } catch {
            Write-Warning "Skip copy (in use): $dest"
        }
    }
}

$depsSource = Join-Path $winRoot "Deps"
$depsTarget = Join-Path $runtimeRoot "Deps"

if ($FetchAppleUsb) {
    $hasAppleDeps = $false
    if (Test-Path $depsSource) {
        $hasAppleDeps = (Get-ChildItem -Path $depsSource -File | Where-Object {
            $_.Name -match 'Apple.*(Mobile|Device).*(Support|Driver).*\.(msi|exe)$'
        } | Measure-Object).Count -gt 0
    }
    if (-not $hasAppleDeps) {
        Write-Host "Fetching Apple USB dependency via winget..."
        powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "bootstrap_apple_usb.ps1") -DownloadOnly -OutputDir $depsSource
        if ($LASTEXITCODE -ne 0) { throw "bootstrap_apple_usb.ps1 failed." }
    } else {
        Write-Host "Apple USB dependency already present in Win/Deps."
    }
}

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

$exe = Join-Path $runtimeDir "WcsNativeWinUI.exe"
if (-not (Test-Path $exe)) {
    throw "Built exe not found after copy: $exe"
}

Write-Host "Building WinUI launcher executable..."
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_native_winui_launcher.ps1")
if ($LASTEXITCODE -ne 0) { throw "build_native_winui_launcher.ps1 failed." }

$launcherCmd = Join-Path $runtimeRoot "wcs_native_winui.cmd"
$launcherContent = @"
@echo off
setlocal
set "ROOT=%~dp0"
pushd "%ROOT%WcsNativeWinUI"
"%ROOT%WcsNativeWinUI\WcsNativeWinUI.exe" %*
set "EC=%ERRORLEVEL%"
popd
exit /b %EC%
"@
Set-Content -Path $launcherCmd -Value $launcherContent -Encoding ascii

Write-Host "WinUI client ready: $exe"
