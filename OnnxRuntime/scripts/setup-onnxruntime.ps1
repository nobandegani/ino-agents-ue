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
#     Public/                        (C / C++ API headers — used at compile time)
#
#   Binaries/ThirdParty/InoOnnxRuntime/
#     Win64/
#       InoOnnxRuntime.dll           (~13 MB — RENAMED from onnxruntime.dll)
#     Android/arm64-v8a/
#       libonnxruntime.so            (~25 MB, CPU + XNNPACK)
#
# Windows rename rationale:
#   UE 5.7 ships multiple conflicting copies of "onnxruntime.dll" through
#   plugins like NNE (NNERuntimeORT) and some Marketplace runtime plugins.
#   Windows' LoadLibrary uses BASE-NAME caching — if any of those copies
#   gets loaded into the process before ours, our FPlatformProcess::GetDllHandle
#   call with our FULL path silently returns the already-loaded handle
#   (UE's older ORT, likely 1.19.x), and our OrtApi::GetApi(ORT_API_VERSION=24)
#   call returns nullptr because that older DLL doesn't implement API 24.
#
#   We dodge the cache entirely by renaming our DLL to a name no other
#   library uses. The InoOnnxModule startup code then uses GetProcAddress
#   on "OrtGetApiBase" to fish out the one entry point we need, and all
#   subsequent ORT calls go through the returned OrtApi vtable — no
#   implicit link against an import library at all. Clean.
#
# Android does NOT need the rename: only one libonnxruntime.so lands in
# the APK's lib/arm64-v8a/ (verified empirically), the linker loads our
# version via libUnreal.so's DT_NEEDED chain, and no cache conflict is
# possible inside an APK with a single copy.
#
# We do NOT stage onnxruntime.lib anywhere — dynamic loading means we
# never link against it, so keeping it would just be dead weight.
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

# Staging destinations. Note there is no Win64 "lib" directory anymore —
# dynamic loading (GetProcAddress on the renamed DLL) means we never link
# against the ORT import library at UE build time.
$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty\InoOnnxRuntime"
$PublicIncDir     = Join-Path $ThirdPartyDir "Public"
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
    (Test-Path (Join-Path $Win64BinStageDir "InoOnnxRuntime.dll")) -and `
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

foreach ($d in @($CacheDir, $PublicIncDir, $Win64BinStageDir, $Arm64BinStageDir)) {
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

# Stage runtime DLL — RENAMED from onnxruntime.dll to InoOnnxRuntime.dll to
# avoid Windows LoadLibrary base-name caching colliding with UE's NNE and
# other plugins that ship their own onnxruntime.dll. See the header comment
# at the top of this file for the full rationale.
#
# We deliberately do NOT stage the import library (.lib) — the runtime
# consumer code uses GetProcAddress to resolve "OrtGetApiBase" from the
# renamed DLL and accesses everything else through the OrtApi vtable
# that returns. No static linking against the library is involved.
$WinDllFile = Join-Path $WinLib "onnxruntime.dll"
if (-not (Test-Path $WinDllFile)) { Write-Error "onnxruntime.dll not found under $WinLib" }

Copy-Item -Path $WinDllFile -Destination (Join-Path $Win64BinStageDir "InoOnnxRuntime.dll") -Force
Write-Host "  [STAGE] onnxruntime.dll -> $Win64BinStageDir\InoOnnxRuntime.dll (renamed for base-name isolation)"

# onnxruntime_providers_shared.dll is not staged in CPU-only mode. When a
# future phase adds DirectML or another shared-EP provider, we'll stage it
# under a unique name too (e.g. InoOnnxRuntime_providers_shared.dll) and
# update InoOnnxModule.cpp to load it alongside the core DLL.

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
