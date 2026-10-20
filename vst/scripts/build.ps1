<#
.SYNOPSIS
    Configures, builds and tests the plugin from a plain PowerShell prompt.

.PARAMETER Config
    Debug or Release (default Release).

.PARAMETER Install
    Copy the built VST3 into the per-user VST3 folder (%LOCALAPPDATA%\Programs\Common\VST3),
    which Reaper and other VST3 hosts scan. No administrator rights needed.
#>
param(
    [ValidateSet("Debug", "Release")] [string] $Config = "Release",
    [switch] $Install
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio C++ build tools not found." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

$preset = $Config.ToLower()
$cmd = "`"$vcvars`" >nul 2>&1 && cmake --preset vs2026 && cmake --build --preset $preset --parallel && ctest --preset $preset"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build or tests failed." }

$vst3 = Join-Path $root "build\SpectrogramDrawFilter_artefacts\$Config\VST3\Spectrogram Draw Filter.vst3"
Write-Host "Built: $vst3"

if ($Install) {
    $dest = "$env:LOCALAPPDATA\Programs\Common\VST3"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -Recurse -Force $vst3 $dest
    Write-Host "Installed to $dest"

    # Hosts pick one copy when several exist, so a stale system-wide copy would shadow this one.
    $systemCopy = "$env:CommonProgramFiles\VST3\Spectrogram Draw Filter.vst3"
    if (Test-Path $systemCopy) {
        Write-Warning "An older copy exists at '$systemCopy' and may be loaded instead. Delete it (needs admin)."
    }
}
