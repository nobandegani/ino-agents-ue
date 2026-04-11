# setup.ps1
#
# One-time setup for the LiteRtLm Bazel build workspace:
#   1. Verifies toolchain prerequisites (bazelisk, MSVC, BAZEL_VC, etc.)
#   2. Ensures the LiteRT-LM submodule is initialized
#   3. Copies LiteRtLm/overlay/* into vendor/LiteRT-LM/
#   4. Updates the submodule's .git/info/exclude so overlay + bazel artifacts
#      don't show as dirty
#
# Idempotent: safe to run repeatedly.

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$LiteRtLmDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$OverlayDir  = Join-Path $LiteRtLmDir "overlay"
$SubmoduleDir = Join-Path $LiteRtLmDir "vendor\LiteRT-LM"
$PluginDir   = (Resolve-Path (Join-Path $LiteRtLmDir "..")).Path

Write-Host "=== LiteRtLm setup ===" -ForegroundColor Cyan
Write-Host "Plugin dir:    $PluginDir"
Write-Host "LiteRtLm dir:  $LiteRtLmDir"
Write-Host "Submodule dir: $SubmoduleDir"
Write-Host ""

#---------------------------------------------------------------------
# 1. Preflight
#---------------------------------------------------------------------
Write-Host "--- Preflight ---" -ForegroundColor Yellow

function Test-Cmd {
    param([string]$Name, [string]$Hint)
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $found) {
        Write-Error "$Name not found on PATH. $Hint"
    }
    Write-Host "  [OK] $Name -> $($found.Source)"
}

Test-Cmd "bazelisk" "Install via: winget install Bazel.Bazelisk"
Test-Cmd "git" "Install Git for Windows"

# BAZEL_VC
if (-not $env:BAZEL_VC) {
    $envValue = [Environment]::GetEnvironmentVariable("BAZEL_VC", "User")
    if (-not $envValue) {
        Write-Error "BAZEL_VC is not set. Set it to your VS install's VC folder, e.g. 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC'"
    }
    $env:BAZEL_VC = $envValue
}
if (-not (Test-Path $env:BAZEL_VC)) {
    Write-Error "BAZEL_VC points at '$env:BAZEL_VC' which does not exist"
}
Write-Host "  [OK] BAZEL_VC -> $env:BAZEL_VC"

# Git Bash path is hardcoded in upstream .bazelrc
$gitBash = "C:\Program Files\Git\bin\bash.exe"
if (-not (Test-Path $gitBash)) {
    Write-Error "Git Bash not found at '$gitBash'. Upstream .bazelrc hardcodes this path."
}
Write-Host "  [OK] Git Bash -> $gitBash"

# Developer Mode
try {
    $dm = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" `
                           -Name AllowDevelopmentWithoutDevLicense -ErrorAction Stop
    if ($dm.AllowDevelopmentWithoutDevLicense -ne 1) {
        Write-Error "Developer Mode is not enabled. Settings > System > For developers > Developer Mode"
    }
    Write-Host "  [OK] Developer Mode enabled"
} catch {
    Write-Error "Developer Mode is not enabled (registry key missing). Settings > System > For developers > Developer Mode"
}

# Long paths (warn only — we mitigate by using a short output base regardless)
try {
    $lp = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
                           -Name LongPathsEnabled -ErrorAction Stop
    if ($lp.LongPathsEnabled -ne 1) {
        Write-Warning "Windows long paths NOT enabled (non-fatal because we use a short output base)."
    } else {
        Write-Host "  [OK] Windows long paths enabled"
    }
} catch {
    Write-Warning "Could not read long paths registry key"
}

# Short Bazel output base: Windows MAX_PATH is 260 chars and MSVC link.exe does
# not transparently use the \\?\ prefix for its input files. LiteRT-LM's Rust
# proc-macro intermediate .rcgu.o filenames alone consume ~220 chars, so the
# path prefix must be short. Upstream CI uses D:/w-<hash>/; we use C:/b/ino/.
$OutputBase = "C:/b/ino"
if (-not (Test-Path $OutputBase)) {
    try {
        New-Item -ItemType Directory -Path $OutputBase -Force -ErrorAction Stop | Out-Null
        Write-Host "  [OK] Created Bazel output base: $OutputBase"
    } catch {
        Write-Error @"
Could not create '$OutputBase': $($_.Exception.Message)

This directory is required to keep Bazel's execution root path short
(Windows MAX_PATH). Create it once, then rerun:

    From an elevated PowerShell (run as Administrator):
        mkdir C:\b
        icacls C:\b /grant '*S-1-5-32-545:(OI)(CI)M'

The first command creates C:\b. The second grants the built-in 'Users' group
Modify rights with inheritance, so subsequent builds run as your normal
(non-admin) user.
"@
    }
} else {
    Write-Host "  [OK] Bazel output base exists: $OutputBase"
}

Write-Host ""

#---------------------------------------------------------------------
# 2. Submodule initialization
#---------------------------------------------------------------------
Write-Host "--- Submodule ---" -ForegroundColor Yellow

if (-not (Test-Path (Join-Path $SubmoduleDir ".git"))) {
    Write-Host "Submodule not initialized, running 'git submodule update --init --recursive'..."
    Push-Location $PluginDir
    try {
        git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }
    } finally {
        Pop-Location
    }
}

$submoduleHead = git -C $SubmoduleDir rev-parse --short HEAD
Write-Host "  [OK] Submodule at $submoduleHead"
Write-Host ""

#---------------------------------------------------------------------
# 3. Apply overlay (copy overlay/* into submodule)
#---------------------------------------------------------------------
Write-Host "--- Overlay ---" -ForegroundColor Yellow

$overlayTopPaths = @()
if (Test-Path $OverlayDir) {
    Get-ChildItem -Path $OverlayDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($OverlayDir.Length + 1)
        $destPath = Join-Path $SubmoduleDir $rel
        $destDir = Split-Path -Parent $destPath
        if (-not (Test-Path $destDir)) {
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
        }
        Copy-Item -Path $_.FullName -Destination $destPath -Force
        Write-Host "  [COPY] $rel"

        $top = ($rel -split '[\\/]')[0]
        if ($top -and -not ($overlayTopPaths -contains $top)) {
            $overlayTopPaths += $top
        }
    }
} else {
    Write-Warning "Overlay directory not found: $OverlayDir"
}

Write-Host ""

#---------------------------------------------------------------------
# 4. Update submodule's .git/info/exclude
#---------------------------------------------------------------------
Write-Host "--- Submodule exclude list ---" -ForegroundColor Yellow

# Resolve the submodule's real .git directory. For a submodule, .git inside
# the submodule is a *file* pointing at <parent>/.git/modules/<path>/. The
# canonical way to resolve it regardless of layout is via git itself.
$gitDir = (& git -C $SubmoduleDir rev-parse --absolute-git-dir 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $gitDir) {
    Write-Warning "Could not resolve submodule git dir via 'git rev-parse --absolute-git-dir'; skipping exclude update"
    $gitDir = $null
} else {
    $gitDir = $gitDir.Trim()
    Write-Host "  .git dir: $gitDir"
}

if ($gitDir) {
    $infoDir = Join-Path $gitDir "info"
    if (-not (Test-Path $infoDir)) {
        New-Item -ItemType Directory -Path $infoDir -Force | Out-Null
    }
    $excludeFile = Join-Path $infoDir "exclude"

    $excludeEntries = @()
    foreach ($p in $overlayTopPaths) {
        $excludeEntries += "/$p/"
    }
    $excludeEntries += "/bazel-*"
    $excludeEntries += "/user.bazelrc"
    $excludeEntries += "/.bazelrc.user"

    $existing = @()
    if (Test-Path $excludeFile) {
        $existing = Get-Content $excludeFile
    }

    foreach ($entry in $excludeEntries) {
        if ($existing -notcontains $entry) {
            Add-Content -Path $excludeFile -Value $entry
            Write-Host "  [ADD] $entry"
        } else {
            Write-Host "  [OK]  $entry (already present)"
        }
    }
}

Write-Host ""
Write-Host "=== Setup complete ===" -ForegroundColor Green
