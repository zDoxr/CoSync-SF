<#
.SYNOPSIS
    Configure, build and test CoSync-SF inside a Visual Studio developer environment.

.EXAMPLE
    ./scripts/build.ps1                      # release configure + build + test
    ./scripts/build.ps1 -Preset debug -NoTest
#>
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'release',
    [switch]$NoTest,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Locate the newest Visual Studio (or Build Tools) with the x64 C++ toolset.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio 2022+ or Build Tools with the C++ workload.' }
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }

# Import the x64 developer environment (cl, link, Windows SDK) into this process.
# vcvars64.bat itself invokes vswhere.exe, so put it on PATH, and keep the batch
# file's stderr inside cmd (PowerShell 5.1 treats native stderr as an error).
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

# Prefer an explicit VCPKG_ROOT; otherwise use the vcpkg bundled with Visual Studio.
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg' }
Write-Host "Visual Studio : $vs"
Write-Host "VCPKG_ROOT    : $env:VCPKG_ROOT"

# Native tools write warnings to stderr; Windows PowerShell 5.1 would turn those
# into terminating errors under 'Stop'. Failures are detected via $LASTEXITCODE.
$ErrorActionPreference = 'Continue'

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    if ($ConfigureOnly) { return }

    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

    if (-not $NoTest) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
    }
}
finally {
    Pop-Location
}
