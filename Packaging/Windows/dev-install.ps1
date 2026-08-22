<#
    UNI 76 - Windows developer install.

    Copies a locally built .vst3 bundle into a user-writable VST3 folder so
    it can be picked up by a DAW during development. This is NOT the
    production installer - COPY_PLUGIN_AFTER_BUILD is intentionally left off
    in the CMake build itself (see Source/Plugin/CMakeLists.txt); this script
    is the explicit, opt-in dev-install step described in docs/BUILD.md.

    Windows has no single officially-recognised per-user VST3 folder the way
    macOS does. This script uses %LOCALAPPDATA%\Programs\Common\VST3 -
    you'll need to add that folder to your DAW's plug-in search paths once.

    Usage:
        pwsh ./Packaging/Windows/dev-install.ps1 -Configuration Release
#>

param(
    [string] $BuildDir = "$PSScriptRoot\..\..\build\windows-release",
    [string] $Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $BuildDir)) {
    Write-Error "Build directory '$BuildDir' does not exist. Configure and build the project first (see docs/BUILD.md)."
    exit 1
}

$candidates = Get-ChildItem -Path $BuildDir -Recurse -Directory -Filter "*.vst3" -ErrorAction SilentlyContinue

$vst3 = $candidates | Where-Object { $_.FullName -match [Regex]::Escape($Configuration) } | Select-Object -First 1
if (-not $vst3) { $vst3 = $candidates | Select-Object -First 1 }

if (-not $vst3) {
    Write-Error "No built .vst3 bundle found under '$BuildDir'. Build the project first (see docs/BUILD.md)."
    exit 1
}

$targetRoot = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3"
New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null

$destination = Join-Path $targetRoot $vst3.Name

Write-Host "Installing:"
Write-Host "  from: $($vst3.FullName)"
Write-Host "  to:   $destination"

if (Test-Path $destination) {
    Remove-Item -Recurse -Force $destination
}

Copy-Item -Recurse -Force $vst3.FullName $destination

Write-Host ""
Write-Host "Done. If your DAW doesn't already scan '$targetRoot', add it to its VST3 search paths."
