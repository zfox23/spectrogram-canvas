<#
.SYNOPSIS
    Configures, builds and tests the plugin from a plain PowerShell prompt.

.PARAMETER Config
    Debug or Release (default Release).

.PARAMETER Install
    Copy the built VST3 into C:\Program Files\Common Files\VST3 (needs an elevated prompt).
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
    $dest = "$env:CommonProgramFiles\VST3"
    Copy-Item -Recurse -Force $vst3 $dest
    Write-Host "Installed to $dest"
}
