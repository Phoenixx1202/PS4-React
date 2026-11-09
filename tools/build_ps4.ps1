<#
.SYNOPSIS
    Build the PS4 React starter and produce one installable PS4 PKG.

.DESCRIPTION
    The OpenOrbis clang in the PacBrew rootfs needs its own glibc, so the build
    runs through chroot. This wrapper resolves paths, bundles the React app,
    builds inside the rootfs and packages PS4-React-Starter.pkg.

.PARAMETER Distro
    WSL distribution name (default Ubuntu-22.04).

.PARAMETER Rootfs
    PacBrew rootfs path inside that distribution.

.PARAMETER Jobs
    Parallel build jobs.

.PARAMETER ErRoot
    Embedded React checkout (Windows path). Defaults to .deps\embeddedReact and
    is cloned at the pinned revision when missing.

.PARAMETER QuickJsRoot
    QuickJS-ng checkout (Windows path). Defaults to .deps\quickjs.

.PARAMETER Sample
    Build a sample from samples\<name> instead of app/. Its app.json supplies
    the title, Title ID, Content ID and output file name.

.PARAMETER DevServer
    Enable the TCP hot-reload receiver on port 9001 for tools/dev_server.mjs.
    Leave it off for packages you plan to distribute.

.PARAMETER NoPackage
    Build the ELF/SELF and launch folder; skip PkgTool.Core.
#>
[CmdletBinding()]
param(
    [string]$Distro = 'Ubuntu-22.04',
    [string]$Rootfs = '/opt/codex-pacbrew-rootfs/rootfs',
    [int]$Jobs = 6,
    [string]$ErRoot = '',
    [string]$QuickJsRoot = '',
    [string]$Sample = '',
    [switch]$DevServer,
    [switch]$NoPackage
)

$ErrorActionPreference = 'Stop'
if ($Jobs -lt 1) { throw 'Jobs must be positive' }

# Native tools write progress and warnings to stderr; with Stop, Windows
# PowerShell promotes those to terminating errors. Check the exit code instead.
function Invoke-Native {
    param([scriptblock]$Command, [string]$Label)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Command
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "$Label failed ($code)" }
}

function To-WslPath([string]$WindowsPath) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $value = (& wsl.exe -d $Distro --cd / -- wslpath -a ($WindowsPath -replace '\\', '/')).Trim()
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "Cannot resolve $WindowsPath in WSL" }
    if ($value.StartsWith("$Rootfs/mnt/")) {
        $value = $value.Substring($Rootfs.Length)
    }
    return $value
}

$ProjectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectUnix = To-WslPath $ProjectPath
if (-not $ProjectUnix.StartsWith('/mnt/')) {
    throw "The project must live on a Windows drive mounted in WSL (got $ProjectUnix)."
}

$AppRelative = 'app'
if ($Sample) {
    if ($Sample -notmatch '^[A-Za-z0-9._-]+$') {
        throw "Invalid sample name: $Sample"
    }
    $AppRelative = "samples/$Sample"
}
$AppDir = Join-Path $ProjectPath $AppRelative
$AppConfigPath = Join-Path $AppDir 'app.json'
if (-not (Test-Path $AppConfigPath)) {
    throw "App config not found: $AppConfigPath"
}
$AppConfig = Get-Content $AppConfigPath -Raw | ConvertFrom-Json
if (-not $AppConfig.titleId -or -not $AppConfig.contentId) {
    throw "app.json must declare titleId and contentId: $AppConfigPath"
}
$TitleId = [string]$AppConfig.titleId
$ContentId = [string]$AppConfig.contentId
$Title = if ($AppConfig.name) { [string]$AppConfig.name } else { 'PS4 React' }
$Version = if ($AppConfig.version) { [string]$AppConfig.version } else { '01.00' }
$OutputName = if ($Sample) { "PS4-React-$Sample.pkg" } else { 'PS4-React-Starter.pkg' }

$lock = Get-Content (Join-Path $ProjectPath 'dependencies.lock.json') -Raw |
    ConvertFrom-Json

if (-not $ErRoot) {
    $ErRoot = Join-Path $ProjectPath '.deps\embeddedReact'
}
if (-not (Test-Path (Join-Path $ErRoot 'engine\include\er_scene.h'))) {
    New-Item -ItemType Directory -Force (Split-Path $ErRoot) | Out-Null
    Write-Host 'Fetching Embedded React...'
    Invoke-Native { git clone $lock.embeddedReact.url $ErRoot } 'Embedded React clone'
    Invoke-Native {
        git -C $ErRoot checkout --detach $lock.embeddedReact.revision
    } 'Embedded React checkout'
}
$ErRoot = (Resolve-Path $ErRoot).Path
$ErRootUnix = To-WslPath $ErRoot

if (-not $QuickJsRoot) {
    $QuickJsRoot = Join-Path $ProjectPath '.deps\quickjs'
}
if (-not (Test-Path (Join-Path $QuickJsRoot 'quickjs.h'))) {
    New-Item -ItemType Directory -Force (Split-Path $QuickJsRoot) | Out-Null
    Write-Host 'Fetching QuickJS-ng 0.15.0...'
    Invoke-Native {
        git clone --depth 1 --branch v0.15.0 $lock.quickjs.url $QuickJsRoot
    } 'QuickJS clone'
}
$QuickJsUnix = To-WslPath $QuickJsRoot

Write-Host "Bundling $AppRelative..."
& (Join-Path $PSScriptRoot 'bundle_react.ps1') -ErRoot $ErRoot -App $AppRelative
if ($LASTEXITCODE -ne 0) { throw "React bundle failed ($LASTEXITCODE)" }

Write-Host "Building $ProjectUnix in $Rootfs ($Distro, $Jobs jobs)..."
Invoke-Native {
    wsl.exe -d $Distro --cd / -u root -- bash "$ProjectUnix/tools/build_ps4_wsl.sh" `
        $Rootfs $Jobs $ProjectUnix $ErRootUnix $QuickJsUnix $AppRelative `
        $([int]$DevServer.IsPresent)
} 'PS4 build'

if (-not $NoPackage) {
    Write-Host "Packaging $Title ($TitleId)..."
    Invoke-Native {
        wsl.exe -d $Distro --cd / -u root -- bash "$ProjectUnix/tools/package_ps4_wsl.sh" `
            $Rootfs $ProjectUnix "$ProjectUnix/build/native/ps4/react" `
            "$ProjectUnix/build/app-react" `
            $TitleId `
            $ContentId `
            $Title `
            "$ProjectUnix/build/$OutputName" `
            $Version
    } 'PS4 packaging'
}

Write-Host ''
Write-Host 'Output:'
Write-Host "  $ProjectPath\build\native\ps4\react\ps4_react_starter"
Write-Host "  $ProjectPath\build\native\ps4\react\eboot.bin"
if (-not $NoPackage) {
    Write-Host "  $ProjectPath\build\$OutputName"
}
