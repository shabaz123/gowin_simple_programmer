# ============================================================================
# EDIT THIS PATH to match your local Gowin Programmer installation.
# This must be the directory containing programmer_cli.exe.
# ============================================================================
$GowinBin = 'C:\DEV\vhd_mounts\tnano\Gowin_Programmer_V1.9.12.04_x64\Programmer\bin'

# Device/cable settings. Change these if your hardware differs.
$Device = 'GW1NR-9C'
$CableIndex = 1

# Usage: .\generate_gowin_svfs.ps1 <path-to-bitstream.fs>
# Produces <bitstream-folder>\svf_generated\sram.svf and flash.svf.
# SVF generation only: this script does not program the FPGA or Flash.

$ErrorActionPreference = 'Stop'

function Show-Usage {
    Write-Host @'
Generate Gowin SRAM and external-Flash SVF files from a .fs bitstream.

USAGE
    .\generate_gowin_svfs.ps1 <bitstream.fs>
    .\generate_gowin_svfs.ps1 -Help

EXAMPLE
    .\generate_gowin_svfs.ps1 "C:\DEV\projects\tang\ftdi_test\double_blinky.fs"

OUTPUT
    <directory-of-bitstream>\svf_generated\sram.svf
    <directory-of-bitstream>\svf_generated\flash.svf

CONFIGURATION
    Edit $GowinBin near the top of this script to locate programmer_cli.exe.
    Device: GW1NR-9C; cable index: 1.

NOTES
    SRAM uses operation index 2; external Flash uses operation index 5.
    This only generates SVFs; it does not execute them on hardware.
    Do not execute the Flash SVF until its content has been reviewed.
'@
}

if ($args.Count -eq 0 -or ($args.Count -eq 1 -and $args[0] -in @('-Help', '--help', '-h', '/?'))) {
    Show-Usage
    exit 0
}
if ($args.Count -ne 1 -or $args[0].StartsWith('-')) {
    Show-Usage
    Write-Error 'Exactly one .fs file path is required.'
    exit 2
}

$FsFile = [string]$args[0]
if ([System.IO.Path]::GetExtension($FsFile) -ine '.fs') {
    throw "Expected a .fs file: $FsFile"
}
if (-not (Test-Path -LiteralPath $FsFile -PathType Leaf)) {
    throw "Bitstream not found: $FsFile"
}
$FsFile = (Resolve-Path -LiteralPath $FsFile).Path

$exe = Join-Path $GowinBin 'programmer_cli.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Gowin CLI not found: $exe. Edit `$GowinBin at the top of the script."
}

$OutputDir = Join-Path (Split-Path -Parent $FsFile) 'svf_generated'
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

# The Gowin CLI emits a fixed output.svf in its own bin directory.
# Temporarily preserve any preexisting file and restore it after each job.
$generated = Join-Path $GowinBin 'output.svf'
$backup = $null
if (Test-Path -LiteralPath $generated) {
    $backup = Join-Path $GowinBin ('.output.svf.backup.' + [guid]::NewGuid().ToString('N'))
    Move-Item -LiteralPath $generated -Destination $backup -ErrorAction Stop
    Write-Host "Preserved existing output.svf as $backup"
}

try {
    foreach ($job in @(@{ Name = 'sram'; Index = 2 }, @{ Name = 'flash'; Index = 5 })) {
        # There must be no output.svf before invoking the CLI. Otherwise a
        # failed generation could be mistaken for a successful one.
        if (Test-Path -LiteralPath $generated) {
            throw "Unexpected output.svf already exists: $generated"
        }

        Write-Host "Generating $($job.Name) SVF (operation index $($job.Index))..."
        Push-Location $GowinBin
        try {
            & $exe --device $Device --operation_index $job.Index --fsFile $FsFile --cable-index $CableIndex --svfcreate
            $exitCode = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }

        if ($exitCode -ne 0) {
            throw "Gowin CLI failed for $($job.Name) with exit code $exitCode."
        }
        if (-not (Test-Path -LiteralPath $generated -PathType Leaf)) {
            throw "Gowin did not create $generated for $($job.Name). Check CLI output."
        }
        if ((Get-Item -LiteralPath $generated).Length -eq 0) {
            throw "Gowin CLI generated an empty $($job.Name) SVF."
        }

        $destination = Join-Path $OutputDir ($job.Name + '.svf')
        Copy-Item -LiteralPath $generated -Destination $destination -Force
        $size = (Get-Item -LiteralPath $destination).Length
        Write-Host "Created: $destination ($size bytes)"
        Remove-Item -LiteralPath $generated -Force
    }
}
finally {
    # Restore the user's original output.svf even if either job fails.
    if ($null -ne $backup -and (Test-Path -LiteralPath $backup)) {
        if (Test-Path -LiteralPath $generated) {
            # Do not overwrite an unexpected newly created output on failure.
            $leftover = Join-Path $GowinBin ('.output.svf.incomplete.' + [guid]::NewGuid().ToString('N'))
            Move-Item -LiteralPath $generated -Destination $leftover
            Write-Warning "Preserved incomplete output at $leftover"
        }
        Move-Item -LiteralPath $backup -Destination $generated
        Write-Host 'Restored original Gowin output.svf.'
    }
}

Write-Host 'Finished generating both SVFs. Do not execute flash.svf until reviewed.'
