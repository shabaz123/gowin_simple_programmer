<#
.SYNOPSIS
    Builds or cleans the tang_programmer project.

.DESCRIPTION
    A helper PowerShell script to configure and build tang_programmer using CMake and MSVC.

.PARAMETER Clean
    Cleans the build directory without compiling.

.PARAMETER Rebuild
    Performs a clean followed by a full rebuild.

.PARAMETER Config
    The build configuration: 'Release' (default) or 'Debug'.

.EXAMPLE
    .\build.ps1
    Builds the Release configuration.

.EXAMPLE
    .\build.ps1 -Clean
    Removes the build directory.

.EXAMPLE
    .\build.ps1 -Rebuild
    Cleans and rebuilds Release configuration.

.EXAMPLE
    .\build.ps1 -Config Debug
    Builds the Debug configuration.
#>

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Rebuild,
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"

# Verify CMake is installed
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Write-Error "CMake was not found in PATH. Please install CMake or add it to PATH."
    exit 1
}

# Clean step
if ($Clean -or $Rebuild) {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning build directory: $BuildDir" -ForegroundColor Yellow
        Remove-Item -Path $BuildDir -Recurse -Force
        Write-Host "Clean complete." -ForegroundColor Green
    } else {
        Write-Host "Build directory does not exist. Nothing to clean." -ForegroundColor Gray
    }

    if ($Clean -and -not $Rebuild) {
        Write-Host "Finished clean operation. (Run .\build.ps1 to build, or .\build.ps1 -Rebuild to clean & build)" -ForegroundColor Cyan
        exit 0
    }
}

# Configure step (if build directory or CMakeCache doesn't exist)
$CMakeCache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $CMakeCache)) {
    Write-Host "Configuring CMake project..." -ForegroundColor Cyan
    & cmake -B $BuildDir -S $ProjectRoot
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configuration failed with exit code $LASTEXITCODE."
        exit $LASTEXITCODE
    }
}

# Build step
Write-Host "Building tang_programmer ($Config)..." -ForegroundColor Cyan
& cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE."
    exit $LASTEXITCODE
}

$OutExe = Join-Path $BuildDir "$Config\tang_programmer.exe"
$OutDll = Join-Path $BuildDir "$Config\tang_programmer.dll"

Write-Host ""
Write-Host "=== Build Successful ===" -ForegroundColor Green
if (Test-Path $OutExe) {
    Write-Host "  Executable : $OutExe" -ForegroundColor White
}
if (Test-Path $OutDll) {
    Write-Host "  Library    : $OutDll" -ForegroundColor White
}
Write-Host ""
