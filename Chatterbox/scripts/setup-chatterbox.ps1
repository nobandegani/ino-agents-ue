# setup-chatterbox.ps1
#
# Dev-time helper: downloads the Chatterbox Turbo ONNX models into the
# project's PersistentDownloadDir so editor-side smoke tests
# (Ino.Chatterbox.LoadModelsTest, SynthTest, RoundTripTest) can find them.
#
# Shipping games do NOT call this script — the UInoChatterboxSubsystem
# handles the download at first-run on the player's device, using the
# same URLs and the same PersistentDownloadDir layout. This script
# exists only so developers can skip that first-run wait during
# iteration.
#
# Usage:
#   ./setup-chatterbox.ps1                       # default variant from VERSION file
#   ./setup-chatterbox.ps1 -Variant q4f16        # override (for mobile-parity tests)
#   ./setup-chatterbox.ps1 -IncludeAuthoring     # also download speech_encoder (voice-embedding extraction)
#
# The four components on HuggingFace (ResembleAI/chatterbox-turbo-ONNX):
#   language_model        T3 backbone; autoregressive text->speech-token decoder
#   embed_tokens          token-embedding lookup; separate file so the LM can be memory-mapped
#   conditional_decoder   S3Gen mel decoder + HiFi-GAN vocoder, combined into one ORT model
#   speech_encoder        reference-audio -> speaker embedding (AUTHORING ONLY)
#
# Each component ships in five quantization variants:
#   fp32, fp16, q4, q4f16, quantized
#
# The variant string is embedded in the filename, e.g. language_model_fp16.onnx.
# Files bigger than ~2 GB on disk spill their weights into a sibling
# <name>.onnx_data file — we always download both when present.
#
# Output layout (relative to project root):
#   Saved/
#     PersistentDownloadDir/
#       InoAgents/
#         Models/
#           Chatterbox/
#             <variant>/
#               language_model_<variant>.onnx
#               language_model_<variant>.onnx_data
#               embed_tokens_<variant>.onnx
#               embed_tokens_<variant>.onnx_data
#               conditional_decoder_<variant>.onnx
#               conditional_decoder_<variant>.onnx_data
#               tokenizer.json
#               config.json
#               generation_config.json
#               speech_encoder_<variant>.onnx        (only if -IncludeAuthoring)
#               speech_encoder_<variant>.onnx_data   (only if -IncludeAuthoring)
#               default_voice.wav                    (only if -IncludeAuthoring; cross-borrowed from
#                                                     onnx-community/chatterbox-ONNX; 24 kHz mono)
#
# UInoChatterboxSubsystem uses FPaths::ProjectPersistentDownloadDir() +
# "InoAgents/Models/Chatterbox/<variant>/" to resolve this path at runtime,
# independent of whether the script or the subsystem populated it.

[CmdletBinding()]
param(
    # Override the variant pinned in CHATTERBOX_VERSION. Useful for pulling
    # both fp16 (desktop tests) and q4f16 (mobile-parity tests) into the
    # same working tree.
    [string] $Variant = $null,

    # Also download the speech encoder (needed for the authoring workflow
    # that converts a reference .wav into a speaker embedding). Off by
    # default — speech_encoder is ~1 GB fp32 and never runs on-device.
    [switch] $IncludeAuthoring,

    # Force re-download even if cached files are present with correct size.
    [switch] $Force
)

$ErrorActionPreference = "Stop"

$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$ChatterboxDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir     = (Resolve-Path (Join-Path $ChatterboxDir "..")).Path
$ProjectDir    = (Resolve-Path (Join-Path $PluginDir "..\..")).Path
$VersionFile   = Join-Path $ChatterboxDir "CHATTERBOX_VERSION"

#---------------------------------------------------------------------
# 1. Load and parse the version pin
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "CHATTERBOX_VERSION not found at $VersionFile"
}

# Read non-comment, non-empty lines. Take the first one as the pin.
$VersionLine = Get-Content $VersionFile `
    | Where-Object { $_ -and ($_.Trim() -notmatch '^\s*#') } `
    | Select-Object -First 1
if (-not $VersionLine) {
    Write-Error "CHATTERBOX_VERSION contains no pin line (only comments). Expected format like 'fp16@main'."
}
$VersionLine = $VersionLine.Trim()

if ($VersionLine -notmatch '^(?<var>[A-Za-z0-9]+)@(?<rev>\S+)$') {
    Write-Error "CHATTERBOX_VERSION line '$VersionLine' does not match the expected '<variant>@<revision>' format."
}
$PinnedVariant  = $Matches['var']
$PinnedRevision = $Matches['rev']

# Command-line -Variant overrides the pin's variant but not the revision.
$ActiveVariant = if ($Variant) { $Variant } else { $PinnedVariant }

$ValidVariants = @('fp32', 'fp16', 'q4', 'q4f16', 'quantized')
if ($ValidVariants -notcontains $ActiveVariant) {
    Write-Error "Variant '$ActiveVariant' is not one of: $($ValidVariants -join ', ')"
}

Write-Host ""
Write-Host "=== Chatterbox model setup ===" -ForegroundColor Cyan
Write-Host "Plugin dir:         $PluginDir"
Write-Host "Project dir:        $ProjectDir"
Write-Host "Pinned variant:     $PinnedVariant"
Write-Host "Active variant:     $ActiveVariant$(if ($Variant) { '  (command-line override)' })"
Write-Host "Revision:           $PinnedRevision"
Write-Host "Include authoring:  $($IncludeAuthoring.IsPresent)"

#---------------------------------------------------------------------
# 2. Compute target directory + URL bases
#---------------------------------------------------------------------
$TargetDir = Join-Path $ProjectDir "Saved\PersistentDownloadDir\InoAgents\Models\Chatterbox\$ActiveVariant"

# Hugging Face URL pattern:
#   https://huggingface.co/<org>/<repo>/resolve/<revision>/<path>
$HfRepo     = "ResembleAI/chatterbox-turbo-ONNX"
$HfBase     = "https://huggingface.co/$HfRepo/resolve/$PinnedRevision"

Write-Host "Target dir:         $TargetDir"
Write-Host "HuggingFace repo:   $HfRepo"
Write-Host ""

if (-not (Test-Path $TargetDir)) {
    New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
}

#---------------------------------------------------------------------
# 3. Build the download list
#---------------------------------------------------------------------
# The four main ONNX components. Each one has a companion .onnx_data
# file on HuggingFace when the weights exceed ~2 GB; we don't pre-check
# and instead try to download both and tolerate a 404 on the _data side.
# (In practice every variant except maybe the tiniest has a _data file.)
$RuntimeComponents  = @('language_model', 'embed_tokens', 'conditional_decoder')
$AuthoringComponents = @('speech_encoder')

# Config + tokenizer files (same for every variant; downloaded into each
# variant's directory for self-containment so the subsystem doesn't have
# to resolve paths out of the variant tree).
$ConfigFiles = @(
    'tokenizer.json',
    'config.json',
    'generation_config.json'
    # Note: tokenizer_config.json and preprocessor_config.json exist
    # upstream but the inference pipeline doesn't use them directly —
    # skip to save a round-trip.
)

# Assemble the list of (sourcePath, destPath) pairs.
$Downloads = New-Object System.Collections.Generic.List[object]

foreach ($comp in $RuntimeComponents) {
    $OnnxFile = "${comp}_${ActiveVariant}.onnx"
    $Downloads.Add([pscustomobject]@{
        SourceUrl = "$HfBase/onnx/$OnnxFile"
        DestPath  = Join-Path $TargetDir $OnnxFile
        Required  = $true
    })
    # .onnx_data companion — usually present for all variants we care about.
    $DataFile = "${comp}_${ActiveVariant}.onnx_data"
    $Downloads.Add([pscustomobject]@{
        SourceUrl = "$HfBase/onnx/$DataFile"
        DestPath  = Join-Path $TargetDir $DataFile
        Required  = $false   # tolerate 404 for tiny variants where weights fit in the .onnx itself
    })
}

if ($IncludeAuthoring) {
    foreach ($comp in $AuthoringComponents) {
        $OnnxFile = "${comp}_${ActiveVariant}.onnx"
        $Downloads.Add([pscustomobject]@{
            SourceUrl = "$HfBase/onnx/$OnnxFile"
            DestPath  = Join-Path $TargetDir $OnnxFile
            Required  = $true
        })
        $DataFile = "${comp}_${ActiveVariant}.onnx_data"
        $Downloads.Add([pscustomobject]@{
            SourceUrl = "$HfBase/onnx/$DataFile"
            DestPath  = Join-Path $TargetDir $DataFile
            Required  = $false
        })
    }

    # Reference audio for authoring/smoke-testing. ResembleAI's turbo
    # repo does NOT ship a default voice file (checked the tree manually
    # in April 2026 — only README/configs/tokenizer and the onnx/
    # directory, no .wav assets). The sibling non-turbo export
    # (onnx-community/chatterbox-ONNX) ships default_voice.wav (714 KB,
    # 24 kHz mono) under MIT, and the speaker-embedding interface is
    # architecturally identical between regular and turbo (same x-vector
    # 192-dim conditioning), so the same reference clip works for
    # priming either encoder. Cross-borrow it here so SynthTest has
    # something to run against without asking every developer to supply
    # their own reference clip.
    #
    # At shipping time this file is NOT used — voice assets get baked
    # at dev time via the authoring pipeline into a small .bin shipped
    # per-voice. This download only covers dev-time smoke-testing of
    # the end-to-end pipeline.
    $Downloads.Add([pscustomobject]@{
        SourceUrl = "https://huggingface.co/onnx-community/chatterbox-ONNX/resolve/main/default_voice.wav"
        DestPath  = Join-Path $TargetDir "default_voice.wav"
        Required  = $true
    })
}

foreach ($cfg in $ConfigFiles) {
    $Downloads.Add([pscustomobject]@{
        SourceUrl = "$HfBase/$cfg"
        DestPath  = Join-Path $TargetDir $cfg
        Required  = $true
    })
}

#---------------------------------------------------------------------
# 4. Download helper with HEAD-size check + resume-style idempotency
#---------------------------------------------------------------------
function Download-Item {
    param(
        [string] $Url,
        [string] $Dest,
        [bool]   $Required
    )

    $Leaf = Split-Path -Leaf $Dest

    # HEAD the URL. Two gotchas with HuggingFace + LFS-backed files:
    #   (1) HEAD against huggingface.co returns a 302 redirect to
    #       cdn-lfs.hf.co. Invoke-WebRequest follows redirects for GET
    #       but NOT for HEAD — we see the 302's headers where
    #       Content-Length is 0 (the redirect body is empty) instead
    #       of the real file size.
    #   (2) Some CDNs omit Content-Length even on the final response.
    # Treat HEAD as best-effort: use it for 404 detection and
    # expected-size reporting when available, but don't rely on it for
    # idempotency.
    $ExpectedSize = 0
    $HeadOk = $false
    try {
        $Head = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing -ErrorAction Stop -MaximumRedirection 5
        $HeadOk = $true
        if ($Head.Headers['Content-Length']) {
            $ExpectedSize = [int64] $Head.Headers['Content-Length'][0]
        }
    } catch {
        $Status = $null
        try { $Status = $_.Exception.Response.StatusCode.Value__ } catch {}
        if ($Status -eq 404) {
            if (-not $Required) {
                Write-Host "  [SKIP ] $Leaf  (optional, 404 — this variant probably inlines weights)" -ForegroundColor DarkGray
                return
            }
            Write-Error "Required file is 404 on HuggingFace: $Url"
        }
        # Any other HEAD error just falls through to the GET below —
        # we'll let the real download report the failure.
    }

    # Sidecar marker file recording size + URL at completion of the last
    # successful download. Our source of truth for cache-hit detection
    # because HF's HEAD doesn't give us a reliable Content-Length.
    $Marker = "$Dest.url"

    if ((Test-Path $Dest) -and (Test-Path $Marker) -and (-not $Force)) {
        $Existing = Get-Item $Dest
        # Stamp format: "<size>\t<url>" (tab-separated, ASCII)
        $Stamp = Get-Content $Marker -Raw -ErrorAction SilentlyContinue
        if ($Stamp) {
            $Parts = $Stamp -split "`t", 2
            if ($Parts.Count -eq 2) {
                $StampedSize = [int64] $Parts[0]
                $StampedUrl  = $Parts[1].Trim()
                if ($Existing.Length -eq $StampedSize -and $StampedUrl -eq $Url) {
                    $MB = [math]::Round($Existing.Length / 1MB, 1)
                    Write-Host "  [CACHE] $Leaf  ($MB MB, stamp-verified)" -ForegroundColor DarkGreen
                    return
                }
            }
        }
        # Fall through — re-download. A partial file from an earlier
        # interrupted run will land here.
        Write-Host "  [STALE] $Leaf  (stamp missing or mismatched; re-downloading)" -ForegroundColor Yellow
    }

    $MBTotal = if ($ExpectedSize -gt 0) { [math]::Round($ExpectedSize / 1MB, 1) } else { '?' }
    Write-Host "  [GET  ] $Leaf  ($MBTotal MB)"
    Write-Host "          -> $Dest"

    # Delete any stale marker before starting so a crash mid-download
    # doesn't leave us with a marker pointing at a partial file.
    if (Test-Path $Marker) {
        Remove-Item $Marker -Force
    }

    # Actual download. Invoke-WebRequest shows a progress bar by default.
    # Use -UseBasicParsing to skip IE COM dependencies (Server Core, CI).
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing

    # Post-download stamp.
    $FinalSize = (Get-Item $Dest).Length
    Set-Content -Path $Marker -Value "$FinalSize`t$Url" -NoNewline -Encoding ASCII
    $MB = [math]::Round($FinalSize / 1MB, 1)
    Write-Host "          ($MB MB written)"
}

#---------------------------------------------------------------------
# 5. Run the downloads
#---------------------------------------------------------------------
Write-Host "--- Downloading $($Downloads.Count) files ---" -ForegroundColor Yellow
foreach ($dl in $Downloads) {
    Download-Item -Url $dl.SourceUrl -Dest $dl.DestPath -Required $dl.Required
}

Write-Host ""

#---------------------------------------------------------------------
# 6. Stamp a .chatterbox_version marker so the subsystem can detect drift
#---------------------------------------------------------------------
$StampPath = Join-Path $TargetDir ".chatterbox_version"
$StampValue = "$ActiveVariant@$PinnedRevision"
Set-Content -Path $StampPath -Value $StampValue -NoNewline -Encoding ASCII

# Print a final summary.
$Present = (Get-ChildItem $TargetDir -File | Measure-Object).Count
Write-Host "=== Chatterbox $ActiveVariant staged ===" -ForegroundColor Green
Write-Host "  $Present files under $TargetDir"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. (Phase B-D) Implement tokenizer + pipeline + subsystem."
Write-Host "  2. Smoke test:  Ino.Chatterbox.LoadModelsTest"
Write-Host "  3. Smoke test:  Ino.Chatterbox.SynthTest"
