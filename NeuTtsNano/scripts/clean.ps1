# clean.ps1
#
# Removes the pre-staged NeuTTS Nano models from PersistentDownloadDir
# and wipes the local .cache/ dir. Safe to run anytime.
#
# Does NOT touch the committed default_voice.nvoice.json under
# Resources/ — that's a plugin asset, not a cache entry.

[CmdletBinding()]
param(
    [string]$ProjectName = "InoProject"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$NeuTtsDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path

$LocalAppData = [Environment]::GetFolderPath("LocalApplicationData")
$StageDir = Join-Path $LocalAppData "$ProjectName\Saved\PersistentDownloadDir\InoAgents\Models\NeuTtsNano"
$CacheDir = Join-Path $NeuTtsDir ".cache"

$Targets = @($StageDir, $CacheDir)

Write-Host "=== NeuTTS Nano clean ===" -ForegroundColor Cyan
foreach ($t in $Targets) {
    if (Test-Path $t) {
        Remove-Item -Recurse -Force $t
        Write-Host "  [REMOVED] $t"
    } else {
        Write-Host "  [ABSENT]  $t"
    }
}
Write-Host ""
Write-Host "Run setup-neutts-nano.ps1 to restage, or let the editor's" -ForegroundColor Green
Write-Host "runtime download fetch them on first LoadModelAsync call."
