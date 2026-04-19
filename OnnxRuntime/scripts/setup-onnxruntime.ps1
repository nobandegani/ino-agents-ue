# setup-onnxruntime.ps1
#
# One-time setup (idempotent) for the ONNX Runtime half of the InoAgents plugin.
#
# Downloads the official Microsoft prebuilt ONNX Runtime binaries for Win64
# and Android arm64-v8a, stages the headers + import lib under
#   Plugins/InoAgents/Source/ThirdParty/InoOnnxRuntime/
# and the runtime .dll / .so files under
#   Plugins/InoAgents/Binaries/ThirdParty/InoOnnxRuntime/
#
# Pinned version lives in Plugins/InoAgents/OnnxRuntime/ONNXRUNTIME_VERSION.
# Bump that file + re-run this script to update.
#
# Artifacts on disk after this runs (assuming version 1.24.3):
#
#   Source/ThirdParty/InoOnnxRuntime/
#     Public/
#       onnxruntime_c_api.h
#       onnxruntime_cxx_api.h
#       onnxruntime_cxx_inline.h
#       onnxruntime_float16.h
#       onnxruntime_run_options_config_keys.h
#       onnxruntime_session_options_config_keys.h
#       cpu_provider_factory.h
#       (... and a few others ORT ships)
#     Win64/
#       onnxruntime.lib              (~4 MB, import library)
#
#   Binaries/ThirdParty/InoOnnxRuntime/
#     Win64/
#       onnxruntime.dll              (~13 MB, CPU provider only for now)
#     Android/arm64-v8a/
#       libonnxruntime.so            (~15 MB, CPU + XNNPACK)
#
# This script explicitly takes the CPU-only Windows build (not the GPU /
# CUDA / TensorRT mega-bundle). Reasons:
#   1. The GPU bundle is ~300 MB and mostly CUDA runtime DLLs we don't want
#      to ship alongside a UE game.
#   2. DirectML (the GPU provider that matches UE's D3D12 renderer) is
#      a separate concern — we add it later via Microsoft's DirectML NuGet
#      or the Direct-ML standalone package, not via the GPU mega-bundle.
#   3. Starting CPU-only keeps Phase 1 lean. Chatterbox Turbo on a modern
#      gaming CPU is ~1-2s first-chunk on CPU, fully usable for validating
#      the integration end-to-end. GPU acceleration is a follow-up phase.
#
# Android's AAR ships one unified libonnxruntime.so per ABI with CPU +
# XNNPACK baked in. That is the right provider set for ARM — XNNPACK is
# measurably faster than CPU on aarch64 and more reliable than NNAPI
# across vendors.

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$OnnxRtDir    = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir    = (Resolve-Path (Join-Path $OnnxRtDir "..")).Path
$VersionFile  = Join-Path $OnnxRtDir "ONNXRUNTIME_VERSION"
$CacheDir     = Join-Path $OnnxRtDir ".cache"

# Staging destinations (mirror the LiteRtLm pattern).
$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty\InoOnnxRuntime"
$PublicIncDir     = Join-Path $ThirdPartyDir "Public"
$Win64LibStageDir = Join-Path $ThirdPartyDir "Win64"
$Win64BinStageDir = Join-Path $PluginDir "Binaries\ThirdParty\InoOnnxRuntime\Win64"
$Arm64BinStageDir = Join-Path $PluginDir "Binaries\ThirdParty\InoOnnxRuntime\Android\arm64-v8a"

#---------------------------------------------------------------------
# 1. Load pinned version
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "ONNXRUNTIME_VERSION file not found at $VersionFile"
}
$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "ONNXRUNTIME_VERSION must be a plain semver triple like '1.24.3'. Got: '$Version'"
}

Write-Host ""
Write-Host "=== ONNX Runtime setup ===" -ForegroundColor Cyan
Write-Host "Version:       $Version (from ONNXRUNTIME_VERSION)"
Write-Host "Plugin dir:    $PluginDir"
Write-Host "OnnxRuntime:   $OnnxRtDir"
Write-Host "Cache dir:     $CacheDir"
Write-Host ""

#---------------------------------------------------------------------
# 2. Resolve download URLs + target cache paths
#---------------------------------------------------------------------
# Windows CPU-only build lives on GitHub Releases.
$WinZipName = "onnxruntime-win-x64-$Version.zip"
$WinZipUrl  = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$WinZipName"
$WinZipPath = Join-Path $CacheDir $WinZipName

# Android AAR is published to Maven Central (not GitHub Releases).
$AndroidAarName = "onnxruntime-android-$Version.aar"
$AndroidAarUrl  = "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/$Version/$AndroidAarName"
$AndroidAarPath = Join-Path $CacheDir $AndroidAarName

#---------------------------------------------------------------------
# 3. Idempotency: if staged binaries already match this version, skip
#---------------------------------------------------------------------
# Write a small VERSION marker alongside staged files so we can detect
# drift cheaply without having to inspect the binaries themselves.
$StampFile = Join-Path $ThirdPartyDir ".ort_version"

if ((Test-Path $StampFile) -and `
    (Test-Path (Join-Path $Win64LibStageDir "onnxruntime.lib")) -and `
    (Test-Path (Join-Path $Win64BinStageDir "onnxruntime.dll")) -and `
    (Test-Path (Join-Path $Arm64BinStageDir "libonnxruntime.so"))) {
    $StampVersion = (Get-Content $StampFile -Raw).Trim()
    if ($StampVersion -eq $Version) {
        Write-Host "--- Already up to date ---" -ForegroundColor Green
        Write-Host "  ONNX Runtime $Version staged."
        Write-Host "  Delete '$StampFile' or bump ONNXRUNTIME_VERSION to force re-stage."
        exit 0
    } else {
        Write-Host "--- Version drift detected: staged=$StampVersion, pinned=$Version. Re-staging. ---" -ForegroundColor Yellow
    }
}

#---------------------------------------------------------------------
# 4. Preflight: tools we need
#---------------------------------------------------------------------
# Invoke-WebRequest + Expand-Archive are built into PowerShell 5.1+, so no
# external dependencies required for the Windows zip. The Android AAR is
# a zip in disguise; Expand-Archive handles it after we rename to .zip.

foreach ($d in @($CacheDir, $PublicIncDir, $Win64LibStageDir, $Win64BinStageDir, $Arm64BinStageDir)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

function Download-IfMissing {
    param([string]$Url, [string]$Dest, [string]$Label)
    if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt 1MB)) {
        Write-Host "  [CACHED] $Label ($(Split-Path $Dest -Leaf))"
        return
    }
    Write-Host "  [DOWNLOAD] $Label"
    Write-Host "             $Url"
    # UseBasicParsing avoids depending on IE rendering, which is absent on
    # Server Core / some CI runners.
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    $sizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($sizeMb MB)"
}

#---------------------------------------------------------------------
# 5. Download
#---------------------------------------------------------------------
Write-Host "--- Downloading artifacts ---" -ForegroundColor Yellow
Download-IfMissing -Url $WinZipUrl       -Dest $WinZipPath       -Label "Windows x64 ORT"
Download-IfMissing -Url $AndroidAarUrl   -Dest $AndroidAarPath   -Label "Android AAR"
Write-Host ""

#---------------------------------------------------------------------
# 6. Extract Windows build
#---------------------------------------------------------------------
Write-Host "--- Extracting Windows artifacts ---" -ForegroundColor Yellow

$WinExtractDir = Join-Path $CacheDir "win-extract-$Version"
if (Test-Path $WinExtractDir) {
    Remove-Item -Recurse -Force $WinExtractDir
}
New-Item -ItemType Directory -Path $WinExtractDir -Force | Out-Null

# -Force overwrites any previous contents without prompting.
Expand-Archive -Path $WinZipPath -DestinationPath $WinExtractDir -Force

# Inside the zip is a top-level folder like "onnxruntime-win-x64-1.24.3/"
# that holds include/, lib/, and sometimes bin/. Resolve it dynamically so
# version bumps don't break this script.
$WinRoot = Get-ChildItem -Path $WinExtractDir -Directory | Select-Object -First 1
if ($null -eq $WinRoot) {
    Write-Error "Expected exactly one top-level folder inside $WinZipName but found none."
}
$WinInclude = Join-Path $WinRoot.FullName "include"
$WinLib     = Join-Path $WinRoot.FullName "lib"

if (-not (Test-Path $WinInclude)) { Write-Error "Missing include/ under $($WinRoot.FullName)" }
if (-not (Test-Path $WinLib))     { Write-Error "Missing lib/ under $($WinRoot.FullName)" }

# Stage headers. Copy everything under include/ — ORT ships a modest set
# of .h files, all of which are part of the public API surface.
Write-Host "  [STAGE] Headers -> $PublicIncDir"
Get-ChildItem -Path $WinInclude -File | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $PublicIncDir $_.Name) -Force
}

# Stage import library (MSVC link time) and runtime DLL (load time).
$WinLibFile = Join-Path $WinLib "onnxruntime.lib"
$WinDllFile = Join-Path $WinLib "onnxruntime.dll"
if (-not (Test-Path $WinLibFile)) { Write-Error "onnxruntime.lib not found under $WinLib" }
if (-not (Test-Path $WinDllFile)) { Write-Error "onnxruntime.dll not found under $WinLib" }

Copy-Item -Path $WinLibFile -Destination (Join-Path $Win64LibStageDir "onnxruntime.lib") -Force
Copy-Item -Path $WinDllFile -Destination (Join-Path $Win64BinStageDir "onnxruntime.dll") -Force
Write-Host "  [STAGE] onnxruntime.lib -> $Win64LibStageDir"
Write-Host "  [STAGE] onnxruntime.dll -> $Win64BinStageDir"

# ORT sometimes ships onnxruntime_providers_shared.dll even in CPU-only
# builds (used by the shared-EP plumbing). Stage it if present; harmless
# if absent.
$WinSharedDll = Join-Path $WinLib "onnxruntime_providers_shared.dll"
if (Test-Path $WinSharedDll) {
    Copy-Item -Path $WinSharedDll -Destination (Join-Path $Win64BinStageDir "onnxruntime_providers_shared.dll") -Force
    Write-Host "  [STAGE] onnxruntime_providers_shared.dll -> $Win64BinStageDir"
}

Write-Host ""

#---------------------------------------------------------------------
# 7. Extract Android AAR
#---------------------------------------------------------------------
# An .aar is a zip. PowerShell's Expand-Archive is strict about the
# extension, so we copy to a .zip first, then extract.
Write-Host "--- Extracting Android artifacts ---" -ForegroundColor Yellow

$AarAsZip = Join-Path $CacheDir "onnxruntime-android-$Version.zip"
Copy-Item -Path $AndroidAarPath -Destination $AarAsZip -Force

$AndroidExtractDir = Join-Path $CacheDir "android-extract-$Version"
if (Test-Path $AndroidExtractDir) {
    Remove-Item -Recurse -Force $AndroidExtractDir
}
New-Item -ItemType Directory -Path $AndroidExtractDir -Force | Out-Null

Expand-Archive -Path $AarAsZip -DestinationPath $AndroidExtractDir -Force

# The AAR layout puts per-ABI .so files at jni/<abi>/libonnxruntime.so.
$ArmSoSrc = Join-Path $AndroidExtractDir "jni\arm64-v8a\libonnxruntime.so"
if (-not (Test-Path $ArmSoSrc)) {
    Write-Error "libonnxruntime.so not found at expected path inside AAR: $ArmSoSrc"
}

Copy-Item -Path $ArmSoSrc -Destination (Join-Path $Arm64BinStageDir "libonnxruntime.so") -Force
Write-Host "  [STAGE] libonnxruntime.so -> $Arm64BinStageDir"

# The Android AAR also contains the same headers as the Windows zip under
# headers/. We already copied them from Windows — no need to re-copy. But
# verify they match to catch packaging surprises.
$AndroidHeaders = Join-Path $AndroidExtractDir "headers"
if (Test-Path $AndroidHeaders) {
    $AndroidHdrCount = (Get-ChildItem -Path $AndroidHeaders -File).Count
    $WinHdrCount     = (Get-ChildItem -Path $PublicIncDir -File).Count
    if ($AndroidHdrCount -ne $WinHdrCount) {
        Write-Host "  [WARN] Android AAR has $AndroidHdrCount header files, Windows zip has $WinHdrCount. Using Windows headers." -ForegroundColor Yellow
    }
}

Write-Host ""

#---------------------------------------------------------------------
# 8. Write version stamp
#---------------------------------------------------------------------
Set-Content -Path $StampFile -Value $Version -NoNewline -Encoding ASCII

Write-Host "=== ONNX Runtime $Version staged successfully ===" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Phase 2: add Source/ThirdParty/InoOnnxRuntime/InoOnnxRuntime.Build.cs"
Write-Host "  2. Phase 2: add Source/ThirdParty/InoOnnxRuntime/InoOnnxRuntime_UPL_Android.xml"
Write-Host "  3. Phase 3-4: wire StartupModule + FInoOnnxSession wrapper"
