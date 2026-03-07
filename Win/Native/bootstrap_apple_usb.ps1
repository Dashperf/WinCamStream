param(
    [switch]$Install,
    [switch]$DownloadOnly,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

if (-not $Install -and -not $DownloadOnly) {
    $DownloadOnly = $true
}

$winRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir -or [string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $winRoot "Deps"
}

function Ensure-Winget {
    $wg = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $wg) {
        throw "winget.exe not found. Install App Installer from Microsoft Store, then retry."
    }
    return $wg.Source
}

function Find-AppleUsbInstaller {
    param([string]$Dir)

    if (-not (Test-Path $Dir)) { return $null }
    $files = Get-ChildItem -Path $Dir -File | Where-Object {
        ($_.Extension -in ".msi", ".exe") -and
        ($_.Name -match "(?i)apple") -and
        ($_.Name -match "(?i)mobile") -and
        ($_.Name -match "(?i)device") -and
        ($_.Name -match "(?i)support|driver")
    }
    if (-not $files) { return $null }
    return $files | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Download-AppleUsbInstaller {
    param([string]$Dir)

    New-Item -ItemType Directory -Path $Dir -Force | Out-Null

    Write-Host "Downloading Apple Mobile Device Support from official winget source..."
    & winget download `
        --id Apple.AppleMobileDeviceSupport `
        --source winget `
        --accept-source-agreements `
        --accept-package-agreements `
        --download-directory $Dir

    if ($LASTEXITCODE -ne 0) {
        throw "winget download failed for Apple.AppleMobileDeviceSupport."
    }

    $installer = Find-AppleUsbInstaller -Dir $Dir
    if (-not $installer) {
        throw "Download completed but installer not found in: $Dir"
    }

    $normalized = Join-Path $Dir "AppleMobileDeviceSupport64$($installer.Extension)"
    if (-not ($installer.FullName -ieq $normalized)) {
        Copy-Item -Path $installer.FullName -Destination $normalized -Force
        Write-Host "Normalized installer copied to: $normalized"
        return Get-Item $normalized
    }

    return $installer
}

function Install-AppleUsbInstaller {
    param([System.IO.FileInfo]$Installer)

    if (-not $Installer) {
        throw "No installer provided."
    }

    Write-Host "Launching elevated installer: $($Installer.FullName)"
    if ($Installer.Extension -ieq ".msi") {
        Start-Process -FilePath "msiexec.exe" -ArgumentList "/i `"$($Installer.FullName)`"" -Verb RunAs -Wait
    } else {
        Start-Process -FilePath $Installer.FullName -Verb RunAs -Wait
    }
}

$null = Ensure-Winget

$installer = Find-AppleUsbInstaller -Dir $OutputDir
if (-not $installer) {
    $installer = Download-AppleUsbInstaller -Dir $OutputDir
} else {
    Write-Host "Apple USB installer already present: $($installer.FullName)"
    $normalized = Join-Path $OutputDir "AppleMobileDeviceSupport64$($installer.Extension)"
    if (-not ($installer.FullName -ieq $normalized)) {
        Copy-Item -Path $installer.FullName -Destination $normalized -Force
        $installer = Get-Item $normalized
        Write-Host "Normalized installer copied to: $normalized"
    }
}

if ($Install) {
    Install-AppleUsbInstaller -Installer $installer
} else {
    Write-Host "Download complete. Installer ready in: $OutputDir"
}
