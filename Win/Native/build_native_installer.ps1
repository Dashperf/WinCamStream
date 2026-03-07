param(
    [string]$Version = "1.0.0",
    [switch]$RebuildRuntime,
    [switch]$FetchAppleUsb,
    [switch]$SkipAutoInstallInno
)

$ErrorActionPreference = "Stop"

$nativeRoot = $PSScriptRoot
$runtimeDir = Join-Path $nativeRoot "Runtime"
$installerScript = Join-Path $nativeRoot "Installer\WinCamStreamNative.iss"
$releaseDir = Join-Path (Split-Path -Parent $nativeRoot) "Release"

if (-not (Test-Path $installerScript)) {
    throw "Installer script not found: $installerScript"
}

function Resolve-Iscc {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
    )

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }

    $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $regBase = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1"
    if (Test-Path $regBase) {
        $installLocation = (Get-ItemProperty -Path $regBase -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
        if ($installLocation) {
            $fromReg = Join-Path $installLocation "ISCC.exe"
            if (Test-Path $fromReg) { return $fromReg }
        }
    }

    return $null
}

if ($RebuildRuntime -or -not (Test-Path (Join-Path $runtimeDir "wcs_native_winui.exe"))) {
    Write-Host "Building native runtime..."
    if ($FetchAppleUsb) {
        powershell -ExecutionPolicy Bypass -File (Join-Path $nativeRoot "build_native_all.ps1") -FetchAppleUsb
    } else {
        powershell -ExecutionPolicy Bypass -File (Join-Path $nativeRoot "build_native_all.ps1")
    }
    if ($LASTEXITCODE -ne 0) { throw "build_native_all.ps1 failed." }
}

$required = @(
    "wcs_native_winui.exe",
    "wcs_native_client.exe",
    "wcs_native_vcam.exe",
    "iproxy.exe",
    "WcsNativeWinUI\WcsNativeWinUI.exe",
    "UnityCapture\Install.bat"
)

foreach ($rel in $required) {
    $p = Join-Path $runtimeDir $rel
    if (-not (Test-Path $p)) {
        throw "Runtime file missing: $p"
    }
}

$iscc = Resolve-Iscc
if (-not $iscc) {
    if ($SkipAutoInstallInno) {
        throw "Inno Setup compiler (ISCC.exe) not found. Install JRSoftware.InnoSetup or remove -SkipAutoInstallInno."
    }

    Write-Host "Inno Setup not found. Installing via winget..."
    winget install --id JRSoftware.InnoSetup --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
    if ($LASTEXITCODE -ne 0) {
        throw "winget install failed for JRSoftware.InnoSetup."
    }

    $iscc = Resolve-Iscc
    if (-not $iscc) {
        throw "ISCC.exe still not found after installation."
    }
}

New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null

$runtimeAbs = (Resolve-Path $runtimeDir).Path
$releaseAbs = (Resolve-Path $releaseDir).Path

Write-Host "Compiling installer with ISCC..."
& $iscc "/DMyAppVersion=$Version" "/DSourceRuntime=$runtimeAbs" "/DOutputDir=$releaseAbs" $installerScript
if ($LASTEXITCODE -ne 0) { throw "ISCC compilation failed." }

$setup = Get-ChildItem -Path $releaseDir -File -Filter ("WinCamStream-Setup-" + $Version + ".exe") | Select-Object -First 1
if (-not $setup) {
    $setup = Get-ChildItem -Path $releaseDir -File -Filter "WinCamStream-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

if (-not $setup) {
    throw "Installer output not found in $releaseDir"
}

Write-Host "Installer ready: $($setup.FullName)"
