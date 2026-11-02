<#
.SYNOPSIS
    Bundle the PS4 React app and bake its assets.

.DESCRIPTION
    Runs the local esbuild + Embedded React asset baker and embeds
    app.bundle.js as a C translation unit. Outputs to .build/react, outside the
    CMake build directory so incremental builds do not delete it.
#>
[CmdletBinding()]
param(
    [string]$ErRoot = '',
    [string]$App = 'app'
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell turns native stderr into error records; with Stop that
# aborts on harmless bundler warnings. Check the exit code explicitly instead.
function Invoke-Native {
    param([scriptblock]$Command, [string]$Label)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Command
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "$Label failed ($code)" }
}

$ProjectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ErRoot) {
    $ErRoot = (Resolve-Path (Join-Path $ProjectPath '.deps\embeddedReact')).Path
}
$appDir = Join-Path $ProjectPath $App
$entryNames = @('index.jsx', 'index.tsx', 'index.js', 'index.ts')
$entryName = $entryNames | Where-Object { Test-Path (Join-Path $appDir $_) } |
    Select-Object -First 1
if (-not $entryName) {
    throw "App not found or has no JS/TS entrypoint: $appDir"
}
& (Join-Path $PSScriptRoot 'apply_embedded_react_patches.ps1') -ErRoot $ErRoot
$jsRoot = Join-Path $ErRoot 'bridges\quickjs\js'
if (-not (Test-Path (Join-Path $jsRoot 'node_modules'))) {
    Write-Host "Installing Embedded React bundler dependencies..."
    Push-Location $jsRoot
    try { Invoke-Native { npm ci --omit=optional --no-audit --no-fund } 'npm ci' }
    finally { Pop-Location }
}

$output = Join-Path $ProjectPath '.build\react'
New-Item -ItemType Directory -Force $output | Out-Null

# Apps may declare render 2560x1440 and surface 1920x1080: Tailwind scales every
# logical pixel by render.width/1280, then the presenter downsamples to the
# surface. On PS4 the engine renders at 1920x1080 directly, so the staged app
# must be compiled with render = surface or the UI is laid out for 2560 and the
# right side falls off-screen.
$stage = Join-Path $output 'app'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $appDir '*') $stage -Recurse -Force

# Samples share samples/assets; stage it next to the app so '../assets/...'
# resolves from the copied sample.
$sharedAssets = Join-Path $ProjectPath 'samples\assets'
if ($App -like 'samples*' -and (Test-Path $sharedAssets)) {
    $stagedAssets = Join-Path (Split-Path $stage) 'assets'
    New-Item -ItemType Directory -Force $stagedAssets | Out-Null
    Copy-Item (Join-Path $sharedAssets '*') $stagedAssets -Recurse -Force
}

# bundle.mjs always resolves index.jsx. A plain JS/TS app gets a tiny
# entrypoint shim so esbuild uses the right loader for the real file.
$stageEntry = Join-Path $stage 'index.jsx'
if (-not (Test-Path $stageEntry)) {
    $shim = "import './$entryName';`n"
    [IO.File]::WriteAllText($stageEntry, $shim,
        (New-Object System.Text.UTF8Encoding($false)))
}

Invoke-Native {
    node (Join-Path $ProjectPath 'tools\ps4_app_config.mjs') `
        (Join-Path $stage 'app.json') 1920 1080
} 'PS4 app config'

# First pass: bundle and discover the font sizes the 1920px layout uses.
Invoke-Native {
    node (Join-Path $ProjectPath 'tools\bundle.mjs') $ErRoot $stage $output
} 'React bundle'
Invoke-Native {
    node (Join-Path $ProjectPath 'tools\ps4_bundle_assets.mjs') `
        $stage (Join-Path $output 'app.bundle.js')
} 'PS4 asset config'
# Second pass: rebake fonts with the discovered sizes.
Invoke-Native {
    node (Join-Path $ProjectPath 'tools\bundle.mjs') $ErRoot $stage $output
} 'React bundle'

Invoke-Native {
    python (Join-Path $ProjectPath 'tools\embed_bundle.py') `
        (Join-Path $output 'app.bundle.js') (Join-Path $output 'bundle.c')
} 'Bundle embedding'

Write-Host "React bundle ready: $output"
