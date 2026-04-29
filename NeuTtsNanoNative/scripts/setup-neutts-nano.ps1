# setup-neutts-nano.ps1
#
# OPTIONAL developer convenience script. Pre-downloads NeuTTS Nano's two
# model files into the local PersistentDownloadDir cache so the editor's
# first LoadModelAsync call doesn't have to wait ~2 minutes for ~1 GB
# of downloads.
#
# Not required for correctness: UInoNeuTtsNanoNativeSubsystem downloads both
# files at runtime on first use. This script just warms the cache for
# faster dev iteration.
#
# Source:
#   neutts-nano-Q4_0.gguf  (195 MB) from neuphonic/neutts-nano-q4-gguf
#   model.onnx             (783 MB) from neuphonic/neucodec-onnx-decoder
#
# Destination:
#   %LOCALAPPDATA%/InoProject/Saved/PersistentDownloadDir/InoAgents/
#       Models/NeuTtsNanoNative/q4/neutts-nano-Q4_0.gguf
#   %LOCALAPPDATA%/InoProject/Saved/PersistentDownloadDir/InoAgents/
#       Models/NeuTtsNanoNative/q4/model.onnx
#
# (Path is derived from FPaths::ProjectPersistentDownloadDir() for the
# InoProject project; if your project name differs, pass -ProjectName.)

[CmdletBinding()]
param(
    [string]$ProjectName = "InoProject",
    [string]$BackboneUrl = "https://huggingface.co/neuphonic/neutts-nano-q4-gguf/resolve/main/neutts-nano-Q4_0.gguf",
    [string]$CodecUrl    = "https://huggingface.co/neuphonic/neucodec-onnx-decoder/resolve/main/model.onnx"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$NeuTtsDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path

# PersistentDownloadDir (what UE's FPaths::ProjectPersistentDownloadDir() returns on Win64):
#   %LOCALAPPDATA%/<ProjectName>/Saved/PersistentDownloadDir/
$LocalAppData = [Environment]::GetFolderPath("LocalApplicationData")
$StageDir = Join-Path $LocalAppData "$ProjectName\Saved\PersistentDownloadDir\InoAgents\Models\NeuTtsNanoNative\q4"

Write-Host ""
Write-Host "=== NeuTTS Nano model pre-stage ===" -ForegroundColor Cyan
Write-Host "Project:       $ProjectName"
Write-Host "Staging dir:   $StageDir"
Write-Host ""

if (-not (Test-Path $StageDir)) {
    New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
}

function Download-IfMissing {
    param([string]$Url, [string]$Dest, [string]$Label, [int]$MinSizeMb)
    if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt ($MinSizeMb * 1MB))) {
        Write-Host "  [CACHED] $Label"
        Write-Host "           $Dest"
        return
    }
    Write-Host "  [DOWNLOAD] $Label"
    Write-Host "             $Url"
    # Download to a .partial file + atomic rename so an interrupted run
    # never leaves a corrupt file in place (matches the runtime download
    # path's invariant).
    $Partial = "$Dest.partial"
    Invoke-WebRequest -Uri $Url -OutFile $Partial -UseBasicParsing
    Move-Item -Path $Partial -Destination $Dest -Force
    $SizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($SizeMb MB)"
}

Download-IfMissing `
    -Url $BackboneUrl `
    -Dest (Join-Path $StageDir "neutts-nano-Q4_0.gguf") `
    -Label "Backbone (195 MB GGUF)" `
    -MinSizeMb 100

Download-IfMissing `
    -Url $CodecUrl `
    -Dest (Join-Path $StageDir "model.onnx") `
    -Label "NeuCodec decoder (783 MB ONNX)" `
    -MinSizeMb 500

Write-Host ""
Write-Host "=== Pre-stage complete ===" -ForegroundColor Green
Write-Host "UInoNeuTtsNanoNativeSubsystem::LoadModelAsync will find these files"
Write-Host "on first use and skip the runtime download."
