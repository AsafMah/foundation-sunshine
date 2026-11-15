# Forks: build the Panel GUI bundle (sunshine-gui.exe + its native plugin) from the committed Panel submodule, so the shipped
# control panel is the fork's (its updater follows FOUNDATION_RELEASE_REPOSITORY, set by the caller) and never the pinned
# upstream release. Mirrors the Panel repository's own Windows build; the Panel resolves foundation-ui as a sibling checkout
# named ds5-ui-m3e, so the submodule is copied next to it (as build-setup-ui.ps1 does).
# usage: build-fork-gui.ps1 -FoundationUi <ds5-ui-m3e checkout, dependencies installed> -Output <new dir>

param(
    [Parameter(Mandatory)][string]$FoundationUi,
    [Parameter(Mandatory)][string]$Output
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
if (-not $env:FOUNDATION_RELEASE_REPOSITORY) { throw 'Set FOUNDATION_RELEASE_REPOSITORY: a fork GUI must update from the fork' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ui = (Resolve-Path -LiteralPath $FoundationUi).Path
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$work = Join-Path (Split-Path $ui -Parent) 'panel-gui'
if (Test-Path -LiteralPath $work) { Remove-Item -Recurse -Force -LiteralPath $work }
$panel = Join-Path $repo 'src_assets/common/sunshine-control-panel'
if (-not (Test-Path -LiteralPath (Join-Path $panel 'src-tauri/Cargo.toml'))) { throw 'The Panel submodule is not checked out' }
New-Item -ItemType Directory -Path $work | Out-Null
$PSNativeCommandUseErrorActionPreference = $false
robocopy $panel $work /E /XD .git node_modules target dist dist-installer /XF .git /NFL /NDL /NJH /NJS /NP | Out-Null
$robocopyExit = $LASTEXITCODE
$PSNativeCommandUseErrorActionPreference = $true
if ($robocopyExit -ge 8) { throw "robocopy failed: $robocopyExit" }
$global:LASTEXITCODE = 0

Push-Location $work
try {
    npm ci --no-audit --no-fund
    npm run build:renderer
    ./scripts/build-native-plugins.ps1 -Configuration Release -Target x86_64-pc-windows-msvc
    cargo build --locked --manifest-path src-tauri/Cargo.toml --release --target x86_64-pc-windows-msvc
    $target = Join-Path $work 'src-tauri/target/x86_64-pc-windows-msvc/release'
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    foreach ($name in @('sunshine-gui.exe', 'alkaidlab-plugin-stylus.dll', 'WebView2Loader.dll')) {
        $file = Join-Path $target $name
        if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $Output -Force }
        elseif ($name -ne 'WebView2Loader.dll') { throw "$name was not produced" }
    }
} finally {
    Pop-Location
}
Write-Output "fork GUI built from the Panel submodule into $Output (updates from $env:FOUNDATION_RELEASE_REPOSITORY)"
