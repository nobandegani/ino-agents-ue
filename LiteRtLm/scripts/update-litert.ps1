# update-litert.ps1
#
# Bump the LiteRT-LM submodule to a new tag and rebuild.
#
# Usage:
#   .\update-litert.ps1 v0.11.0

param(
    [Parameter(Mandatory=$true)]
    [string]$Tag
)

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LiteRtLmDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$SubmoduleDir = Join-Path $LiteRtLmDir "vendor\LiteRT-LM"

Write-Host "=== Updating LiteRT-LM to $Tag ===" -ForegroundColor Cyan

Push-Location $SubmoduleDir
try {
    git fetch --tags
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }

    git checkout $Tag
    if ($LASTEXITCODE -ne 0) { throw "git checkout $Tag failed (does the tag exist?)" }

    $newSha = git rev-parse HEAD
    Write-Host "New submodule SHA: $newSha"
} finally {
    Pop-Location
}

# Update the plain-text tag marker
Set-Content -Path (Join-Path $LiteRtLmDir "LITERT_LM_TAG") -Value "$Tag`n" -NoNewline:$false

Write-Host ""
Write-Host "=== Rebuilding ===" -ForegroundColor Cyan
& (Join-Path $ScriptDir "build-win64.ps1")
if ($LASTEXITCODE -ne 0) { throw "rebuild failed" }

Write-Host ""
Write-Host "=== Update complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Verify the plugin still loads the new DLL in UE"
Write-Host "  2. Commit:  git add LiteRtLm/vendor/LiteRT-LM LiteRtLm/LITERT_LM_TAG"
