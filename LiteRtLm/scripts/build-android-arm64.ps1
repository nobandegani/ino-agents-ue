# build-android-arm64.ps1
#
# Build libLiteRtLm.so from source for Android arm64-v8a and stage artifacts
# into the InoAgents plugin.
#
# Runs scripts/setup.ps1 first (idempotent), then bazelisk build with
# --config=android_arm64, then copies the resulting .so + prebuilt GPU .so
# files + headers into Source/ThirdParty and Binaries.

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LiteRtLmDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$SubmoduleDir = Join-Path $LiteRtLmDir "vendor\LiteRT-LM"
$PluginDir    = (Resolve-Path (Join-Path $LiteRtLmDir "..")).Path

#---------------------------------------------------------------------
# 1. Preflight: locate an Android NDK r28b or newer
#---------------------------------------------------------------------
# LiteRT-LM's Bazel config requires NDK r28b or newer (per its
# docs/getting-started/build-and-run.md). We auto-detect the newest
# NDK installed under %LOCALAPPDATA%\Android\Sdk\ndk\ and point
# ANDROID_NDK_HOME at it for this build's child Bazel invocation only.
# The user's env is NOT modified.
#
# Note: UE 5.7 itself uses NDK 27.2.12479018. We intentionally do NOT
# use that for Bazel because it is too old for LiteRT-LM. The two
# NDKs coexist fine under Sdk\ndk\ as separate subdirectories.
$NdkBaseDir = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk"
if (-not (Test-Path $NdkBaseDir)) {
    throw "Android NDK base directory not found: $NdkBaseDir. Install the Android SDK via Android Studio first."
}

# Pick the newest NDK >= 28. Directory names are like "28.0.12345678".
$NdkCandidates = Get-ChildItem $NdkBaseDir -Directory |
    Where-Object { $_.Name -match '^(\d+)\.' -and [int]$Matches[1] -ge 28 } |
    Sort-Object -Descending @{Expression = { [version]$_.Name }}

if ($NdkCandidates.Count -eq 0) {
    Write-Host ""
    Write-Host "No Android NDK r28 or newer found under $NdkBaseDir." -ForegroundColor Red
    Write-Host "Installed NDK versions:" -ForegroundColor Yellow
    Get-ChildItem $NdkBaseDir -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
    Write-Host ""
    Write-Host "Install NDK r28c (or newer) via Android Studio:" -ForegroundColor Yellow
    Write-Host "  Tools -> SDK Manager -> SDK Tools -> NDK (Side by side)" -ForegroundColor Yellow
    Write-Host "  check 'Show Package Details', select version 28.x or newer, Apply." -ForegroundColor Yellow
    throw "Android NDK r28b+ required but not installed"
}

$AndroidNdkHome = $NdkCandidates[0].FullName

# CRITICAL: Bazel's rules_android_ndk has a Windows path-slash bug
# (bazelbuild/rules_android_ndk@rules.bzl:106-116): it concatenates
# ndk_path + "/" + subdir, then tries to strip ndk_path from the
# output of str(ctx.path(...)) — but str() returns forward slashes
# while ndk_path has backslashes, so the strip fails and Bazel sees
# an absolute path like "C:/Users/.../AndroidVersion.txt" as a
# symlink destination and errors with "Cannot write outside of the
# repository directory for path ...".
#
# Fix: pass ANDROID_NDK_HOME with forward slashes. Bazel's path
# handling treats forward slashes consistently across platforms, so
# the string comparison in rules_android_ndk works correctly.
$AndroidNdkHome = $AndroidNdkHome.Replace('\', '/')

Write-Host ""
Write-Host "=== Android build preflight ===" -ForegroundColor Cyan
Write-Host "  ANDROID_NDK_HOME: $AndroidNdkHome"
$env:ANDROID_NDK_HOME = $AndroidNdkHome

#---------------------------------------------------------------------
# 2. Run setup (idempotent — same overlay as Win64)
#---------------------------------------------------------------------
& (Join-Path $ScriptDir "setup.ps1")
if ($LASTEXITCODE -ne 0) { throw "setup.ps1 failed" }

#---------------------------------------------------------------------
# 3. Bazel build
#---------------------------------------------------------------------
# Same defines as Win64 (see build-win64.ps1 for the full rationale):
#   --define=litert_link_capi_so=true  — link libLiteRt.so dynamically
#                                         so GPU accelerator .so files
#                                         share one LiteRT instance
#   --define=resolve_symbols_in_exec=false — our output is a .so, not
#                                             an executable
#
# Additional Android specifics:
#   --config=android_arm64  — picks the arm64-v8a toolchain via
#                              upstream's .bazelrc
#   --define=xnn_enable_avxvnniint8=false — already set by
#                                            build:android in upstream
#                                            .bazelrc (required for
#                                            clang < 20)

$BazelOutputBase = "C:/b/ino-android"   # different from Win64 to avoid
                                         # cache-key conflicts between the
                                         # two platform builds
$BazelDiskCache  = "C:/b/ino-android-cache"

Write-Host ""
Write-Host "=== Bazel build (Android arm64) ===" -ForegroundColor Cyan
Write-Host "Working dir:  $SubmoduleDir"
Write-Host "Output base:  $BazelOutputBase"
Write-Host "Disk cache:   $BazelDiskCache"
Write-Host "Target:       //ino:LiteRtLm"
Write-Host ""

Push-Location $SubmoduleDir
try {
    # --host_cxxopt=/std:c++20 — upstream's build:android sets
    #   --host_cxxopt=-std=c++20
    # which assumes the build host uses clang syntax. On Windows the
    # host is MSVC (cl.exe), which ignores -std=c++20 with a D9002
    # warning and falls back to the default (C++14), failing absl's
    # policy_checks.h C++17 minimum. Passing /std:c++20 additionally
    # gives MSVC the correct flag — Bazel accumulates cxxopts, so both
    # are passed to the compiler and MSVC uses whichever it recognises
    # (/std:c++20). Upstream CI only cross-compiles Android from Linux,
    # so this Windows host quirk isn't in their config.
    & bazelisk --output_base=$BazelOutputBase `
        build //ino:LiteRtLm `
        --config=android_arm64 `
        --disk_cache=$BazelDiskCache `
        --define=litert_link_capi_so=true `
        --define=resolve_symbols_in_exec=false `
        --host_cxxopt=/std:c++20 `
        --verbose_failures
    if ($LASTEXITCODE -ne 0) {
        throw "bazelisk build failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}

#---------------------------------------------------------------------
# 4. Stage artifacts into plugin
#---------------------------------------------------------------------
# UE's Android packaging picks up native .so files placed under
# Plugins/<Name>/Binaries/ThirdParty/<Module>/Android/arm64-v8a/
# via the Build.cs RuntimeDependencies + PublicAdditionalLibraries.
# The APK builder copies them into the APK's lib/arm64-v8a/ directory
# where Android's dynamic linker picks them up at process startup.
Write-Host ""
Write-Host "=== Staging artifacts ===" -ForegroundColor Cyan

$BazelBinIno      = Join-Path $SubmoduleDir "bazel-bin\ino"
$Arm64BinDst      = Join-Path $PluginDir "Binaries\ThirdParty\InoAgentsLibrary\Android\arm64-v8a"
$Arm64LibDst      = Join-Path $PluginDir "Source\ThirdParty\InoAgentsLibrary\Android\arm64-v8a"
$PublicIncDst     = Join-Path $PluginDir "Source\ThirdParty\InoAgentsLibrary\Public\litert\lm"
$PrebuiltAndroid  = Join-Path $SubmoduleDir "prebuilt\android_arm64"

foreach ($d in @($Arm64BinDst, $Arm64LibDst, $PublicIncDst)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

# Our Bazel-built libLiteRtLm.so
# Bazel's cc_binary(linkshared=1) produces the .so with the prefix "lib"
# automatically on non-Windows platforms.
$SoSrcCandidates = @(
    (Join-Path $BazelBinIno "libLiteRtLm.so"),
    (Join-Path $BazelBinIno "LiteRtLm.so"),
    (Join-Path $BazelBinIno "libino_LiteRtLm.so")
)
$SoSrc = $null
foreach ($c in $SoSrcCandidates) {
    if (Test-Path $c) { $SoSrc = $c; break }
}
if (-not $SoSrc) {
    Write-Warning "libLiteRtLm.so not found. Inspecting bazel-bin/ino for actual outputs:"
    Get-ChildItem $BazelBinIno 2>$null | ForEach-Object { Write-Host "  $($_.Name)" }
    throw "libLiteRtLm.so not produced by Bazel"
}
Copy-Item -Path $SoSrc -Destination (Join-Path $Arm64BinDst "libLiteRtLm.so") -Force
Write-Host "  [STAGE] libLiteRtLm.so (from $(Split-Path $SoSrc -Leaf)) -> $Arm64BinDst"

# libLiteRt.so — LiteRT core runtime, dynamically linked by libLiteRtLm.so
# Bazel produces this as part of the litert_link_capi_so=true build.
$LiteRtSoSrc = Join-Path $BazelBinIno "libLiteRt.so"
if (Test-Path $LiteRtSoSrc) {
    Copy-Item -Path $LiteRtSoSrc -Destination (Join-Path $Arm64BinDst "libLiteRt.so") -Force
    Write-Host "  [STAGE] libLiteRt.so -> $Arm64BinDst"
} else {
    # Some builds produce it in a different location — scan for it.
    $Found = Get-ChildItem $BazelBinIno -Filter "libLiteRt.so" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Found) {
        Copy-Item -Path $Found.FullName -Destination (Join-Path $Arm64BinDst "libLiteRt.so") -Force
        Write-Host "  [STAGE] libLiteRt.so (from $($Found.FullName)) -> $Arm64BinDst"
    } else {
        Write-Warning "libLiteRt.so not found in bazel-bin — Android runtime linking may fail"
    }
}

# Prebuilt GPU accelerator + constraint provider .so files from upstream
# prebuilt/android_arm64/. These are dynamically loaded by the LiteRT
# engine at runtime (via dlopen with SharedLibrary::Load).
#
# Android has TWO GPU accelerator paths:
#   WebGPU (via Dawn)     — newer, modern GPUs (Adreno, Mali)
#   OpenCL                — broader compatibility, older GPUs
# Ship both so the LiteRT engine can pick whichever works on the
# target device.
$AndroidPrebuiltSoFiles = @(
    "libGemmaModelConstraintProvider.so",
    "libLiteRtGpuAccelerator.so",
    "libLiteRtOpenClAccelerator.so",
    "libLiteRtTopKOpenClSampler.so",
    "libLiteRtTopKWebGpuSampler.so",
    "libLiteRtWebGpuAccelerator.so"
)
foreach ($so in $AndroidPrebuiltSoFiles) {
    $src = Join-Path $PrebuiltAndroid $so
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination (Join-Path $Arm64BinDst $so) -Force
        Write-Host "  [STAGE] $so -> $Arm64BinDst"
    } else {
        Write-Warning "Prebuilt not found: $src"
    }
}

# Headers — same as Win64 (shared across platforms)
$headerSrc = Join-Path $SubmoduleDir "c\engine.h"
if (Test-Path $headerSrc) {
    Copy-Item -Path $headerSrc -Destination (Join-Path $PublicIncDst "engine.h") -Force
    Write-Host "  [STAGE] engine.h -> $PublicIncDst"
} else {
    Write-Warning "Expected header not found at $headerSrc"
}

Write-Host ""
Write-Host "=== Android build complete ===" -ForegroundColor Green
Write-Host "Next steps:"
Write-Host "  1. Rebuild the UE project for Android (Package -> Android)"
Write-Host "  2. Install APK on device"
Write-Host "  3. Test with backend=cpu first, then backend=gpu"
