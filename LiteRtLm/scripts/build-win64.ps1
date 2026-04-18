# build-win64.ps1
#
# Build LiteRtLm.dll from source for Windows x64 and stage artifacts into the
# InoAgents plugin.
#
# Runs scripts/setup.ps1 first (idempotent), then bazelisk build, then copies
# the resulting DLL + import lib + headers into Source/ThirdParty and Binaries.

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$LiteRtLmDir  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$SubmoduleDir = Join-Path $LiteRtLmDir "vendor\LiteRT-LM"
$PluginDir    = (Resolve-Path (Join-Path $LiteRtLmDir "..")).Path

#---------------------------------------------------------------------
# 1. Run setup
#---------------------------------------------------------------------
& (Join-Path $ScriptDir "setup.ps1")
if ($LASTEXITCODE -ne 0) { throw "setup.ps1 failed" }

#---------------------------------------------------------------------
# 2. Bazel build
#---------------------------------------------------------------------
# Match upstream CI (.github/workflows/ci-build-win.yml:117) as closely as
# possible:
#
#   - Short --output_base to keep intermediate paths under Windows MAX_PATH
#     (upstream uses D:/w-<hash>/; we use C:/b/ino which is even shorter)
#   - --disk_cache for persistent action caching across workspaces and expunges
#   - Do NOT pass --config=windows explicitly — upstream's .bazelrc has
#     'build --enable_platform_specific_config' which auto-applies the windows
#     config on Windows. Passing it ourselves causes duplicate-expansion warnings.
#   - --build_tag_filters=-nowindows to exclude any targets upstream marks as
#     non-Windows (parity with upstream CI's behavior)
#   - Clear ANDROID_NDK_HOME for the invocation — upstream's CI does this to
#     avoid androidndk rules trying to create symlinks in a host NDK install.
#     Only affects this child process; user's global env is untouched.

$BazelOutputBase = "C:/b/ino"
$BazelDiskCache  = "C:/b/ino-cache"

Write-Host ""
Write-Host "=== Bazel build ===" -ForegroundColor Cyan
Write-Host "Working dir:  $SubmoduleDir"
Write-Host "Output base:  $BazelOutputBase"
Write-Host "Disk cache:   $BazelDiskCache"
Write-Host "Target:       //ino:LiteRtLm"
Write-Host ""

# Clear ANDROID_NDK_HOME for this child process only (does not touch user env)
$env:ANDROID_NDK_HOME = ""

Push-Location $SubmoduleDir
try {
    & bazelisk --output_base=$BazelOutputBase `
        build //ino:LiteRtLm `
        --disk_cache=$BazelDiskCache `
        --build_tag_filters=-nowindows `
        --define=litert_link_capi_so=true `
        --define=resolve_symbols_in_exec=false `
        --verbose_failures
    if ($LASTEXITCODE -ne 0) {
        throw "bazelisk build failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}

#---------------------------------------------------------------------
# 3. Stage artifacts into plugin
#---------------------------------------------------------------------
Write-Host ""
Write-Host "=== Staging artifacts ===" -ForegroundColor Cyan

$BazelBinIno = Join-Path $SubmoduleDir "bazel-bin\ino"
$Win64BinDst = Join-Path $PluginDir "Binaries\ThirdParty\InoAgentsLibrary\Win64"
$Win64LibDst = Join-Path $PluginDir "Source\ThirdParty\InoAgentsLibrary\Win64"
$PublicIncDst = Join-Path $PluginDir "Source\ThirdParty\InoAgentsLibrary\Public\litert\lm"

foreach ($d in @($Win64BinDst, $Win64LibDst, $PublicIncDst)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

# LiteRtLm.dll
$dllSrc = Join-Path $BazelBinIno "LiteRtLm.dll"
if (-not (Test-Path $dllSrc)) {
    Write-Warning "Expected DLL not at $dllSrc. Inspecting bazel-bin/ino for actual outputs:"
    Get-ChildItem $BazelBinIno 2>$null | ForEach-Object { Write-Host "  $($_.Name)" }
    throw "LiteRtLm.dll not produced by Bazel"
}
Copy-Item -Path $dllSrc -Destination (Join-Path $Win64BinDst "LiteRtLm.dll") -Force
Write-Host "  [STAGE] LiteRtLm.dll -> $Win64BinDst"

# libGemmaModelConstraintProvider.dll — an upstream LiteRT-LM prebuilt binary
# (from vendor/LiteRT-LM/prebuilt/windows_x86_64/) that LiteRtLm.dll depends
# on at runtime. Bazel stages it alongside our DLL via a symlink because some
# cc_library target in the //c:engine_cpu dep graph declares it as data. We
# must copy the resolved symlink target into the plugin's runtime staging dir
# so UE's delay-load finds it.
$gemmaConstraintSrc = Join-Path $BazelBinIno "libGemmaModelConstraintProvider.dll"
if (-not (Test-Path $gemmaConstraintSrc)) {
    throw "libGemmaModelConstraintProvider.dll not found at $gemmaConstraintSrc (Bazel should have staged it)"
}
# Copy-Item -Path with a symlink source copies the resolved target, not the
# link itself — which is exactly what we want.
Copy-Item -Path $gemmaConstraintSrc `
          -Destination (Join-Path $Win64BinDst "libGemmaModelConstraintProvider.dll") `
          -Force
Write-Host "  [STAGE] libGemmaModelConstraintProvider.dll -> $Win64BinDst"

# Import lib (Bazel may name it LiteRtLm.if.lib, LiteRtLm.dll.if.lib, or LiteRtLm.lib)
$libCandidates = @(
    "LiteRtLm.if.lib",
    "LiteRtLm.dll.if.lib",
    "LiteRtLm.lib"
)
$libSrc = $null
foreach ($c in $libCandidates) {
    $p = Join-Path $BazelBinIno $c
    if (Test-Path $p) { $libSrc = $p; break }
}
if (-not $libSrc) {
    Write-Warning "Import library not found under any expected name in $BazelBinIno"
    Get-ChildItem $BazelBinIno | ForEach-Object { Write-Host "  $($_.Name)" }
} else {
    Copy-Item -Path $libSrc -Destination (Join-Path $Win64LibDst "LiteRtLm.lib") -Force
    Write-Host "  [STAGE] $(Split-Path $libSrc -Leaf) -> $Win64LibDst\LiteRtLm.lib"
}

# GPU accelerator prebuilt DLLs — shipped by upstream in prebuilt/windows_x86_64/.
# These are dynamically loaded by the LiteRT engine at runtime when backend="gpu"
# is requested. The engine's SharedLibrary::Load converts .so → .dll on Windows
# and calls LoadLibraryA, so the DLLs just need to be findable (same directory as
# the host DLL or on PATH).
#
# libLiteRt.dll:                    LiteRT core runtime (the GPU DLLs import from this)
# libLiteRtWebGpuAccelerator.dll:   WebGPU → D3D12 GPU accelerator
# libLiteRtTopKWebGpuSampler.dll:   GPU-side top-K sampling
$GpuPrebuiltDir = Join-Path $SubmoduleDir "prebuilt\windows_x86_64"
$GpuDlls = @(
    "libLiteRt.dll",
    "libLiteRtWebGpuAccelerator.dll",
    "libLiteRtTopKWebGpuSampler.dll"
)
foreach ($dll in $GpuDlls) {
    $src = Join-Path $GpuPrebuiltDir $dll
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination (Join-Path $Win64BinDst $dll) -Force
        Write-Host "  [STAGE] $dll -> $Win64BinDst"
    } else {
        Write-Warning "GPU prebuilt not found: $src (GPU backend will not be available)"
    }
}

# Headers
$headerSrc = Join-Path $SubmoduleDir "c\engine.h"
if (Test-Path $headerSrc) {
    Copy-Item -Path $headerSrc -Destination (Join-Path $PublicIncDst "engine.h") -Force
    Write-Host "  [STAGE] engine.h -> $PublicIncDst"
} else {
    Write-Warning "Expected header not found at $headerSrc"
}

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Green
