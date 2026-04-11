# clean.ps1
#
# Wipe the Bazel build cache for this workspace. Use when you want to force a
# full cold rebuild (e.g. after tooling changes, corrupted cache, etc.).
#
# Runs 'bazelisk clean --expunge' inside the LiteRT-LM submodule, which is
# where the bazel-* symlinks and output artifacts actually live.

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LiteRtLmDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$SubmoduleDir = Join-Path $LiteRtLmDir "vendor\LiteRT-LM"

if (-not (Test-Path $SubmoduleDir)) {
    Write-Warning "Submodule not initialized: $SubmoduleDir"
    exit 0
}

$BazelOutputBase = "C:/b/ino"

Push-Location $SubmoduleDir
try {
    Write-Host "Running 'bazelisk --output_base=$BazelOutputBase clean --expunge'..." -ForegroundColor Yellow
    & bazelisk --output_base=$BazelOutputBase clean --expunge
    if ($LASTEXITCODE -ne 0) {
        throw "bazelisk clean failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}

Write-Host "=== Clean complete ===" -ForegroundColor Green
