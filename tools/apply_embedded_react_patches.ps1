<#
.SYNOPSIS
    Apply the pinned Embedded React patches to a checkout.

.DESCRIPTION
    The build uses Embedded React at the revision and patch stack recorded in
    dependencies.lock.json. The image API needs the image-unload patch (it adds
    er_image_load_argb/er_image_unload), and the layout/text fixes are part of
    the engine apps expect. This script is idempotent: if the unload API is
    already present, it does nothing.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ErRoot
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$lockPath = Join-Path $root 'dependencies.lock.json'
$marker = Join-Path $ErRoot 'engine\include\er_scene.h'
if (-not (Test-Path $lockPath)) { throw "Missing lock file: $lockPath" }
if (-not (Test-Path $marker)) { throw "Not an Embedded React checkout: $ErRoot" }

if (Select-String -Path $marker -Pattern 'er_image_unload' -Quiet) {
    Write-Host 'Embedded React patches already applied.'
    return
}

$lock = Get-Content $lockPath -Raw | ConvertFrom-Json
$patches = $lock.embeddedReact.patches
if (-not $patches) { throw 'dependencies.lock.json has no embeddedReact patches' }

foreach ($relative in $patches) {
    $patch = Join-Path $root $relative
    if (-not (Test-Path $patch)) { throw "Missing patch: $patch" }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    git -C $ErRoot apply --ignore-whitespace $patch
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "Failed to apply $relative" }
    Write-Host "applied $relative"
}
