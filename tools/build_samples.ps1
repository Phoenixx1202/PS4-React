<#
.SYNOPSIS
    Build every sample into samples-pkg/.
#>
[CmdletBinding()]
param(
    [string]$Distro = 'Ubuntu-22.04',
    [string]$Rootfs = '/opt/codex-pacbrew-rootfs/rootfs',
    [int]$Jobs = 6,
    [string]$ErRoot = '',
    [string]$QuickJsRoot = ''
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $project 'samples-pkg'
New-Item -ItemType Directory -Force $output | Out-Null
Remove-Item (Join-Path $output '*.pkg') -Force -ErrorAction SilentlyContinue

$samples = Get-ChildItem (Join-Path $project 'samples') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'app.json') } |
    Sort-Object Name
if (-not $samples) { throw 'No samples found.' }

foreach ($sample in $samples) {
    Write-Host ''
    Write-Host "=== $($sample.Name) ==="
    & (Join-Path $PSScriptRoot 'build_ps4.ps1') `
        -Distro $Distro -Rootfs $Rootfs -Jobs $Jobs `
        -ErRoot $ErRoot -QuickJsRoot $QuickJsRoot -Sample $sample.Name
    if ($LASTEXITCODE -ne 0) { throw "Sample $($sample.Name) failed ($LASTEXITCODE)" }

    $pkg = Join-Path $project "build/PS4-React-$($sample.Name).pkg"
    if (-not (Test-Path $pkg)) { throw "Missing sample PKG: $pkg" }
    Copy-Item $pkg (Join-Path $output "PS4-React-$($sample.Name).pkg") -Force
}

Write-Host ''
Write-Host 'Sample packages:'
Get-ChildItem $output -Filter *.pkg | Select-Object Name, Length |
    Format-Table -AutoSize
