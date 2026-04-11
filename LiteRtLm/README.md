# LiteRtLm build workspace

This directory is a self-contained Bazel build workspace for producing
`LiteRtLm.dll` from source for the InoAgents Unreal Engine plugin.

## Layout

```
LiteRtLm/
├── LITERT_LM_TAG           Plain-text pinned upstream tag (e.g. v0.10.1)
├── README.md               This file
├── overlay/                Files staged into the submodule before each build
│   └── ino/
│       ├── BUILD.bazel     Defines //ino:LiteRtLm target (cc_binary, linkshared=1)
│       └── LiteRtLm_exports.cc   Minimal stub source file
├── scripts/
│   ├── setup.ps1           One-time preflight + overlay application
│   ├── build-win64.ps1     Build + stage artifacts into the plugin
│   ├── update-litert.ps1   Bump submodule to a new tag and rebuild
│   └── clean.ps1           Wipe Bazel cache for this workspace
└── vendor/
    └── LiteRT-LM/          Git submodule → https://github.com/google-ai-edge/LiteRT-LM
```

## One-time setup (new dev machine)

1. Enable Developer Mode: Settings → System → For developers → Developer Mode → On
2. Install Visual Studio 2022 with the C++ workload
3. Install bazelisk: `winget install Bazel.Bazelisk`
4. Install Git for Windows (must be at `C:\Program Files\Git`)
5. Install Python 3 (any 3.10+)
6. Set `BAZEL_VC` user environment variable to `<VS install>\VC`, e.g.:
   ```
   C:\Program Files\Microsoft Visual Studio\2022\Community\VC
   ```
   (single backslashes — raw registry string, no escaping)
7. Run `.\scripts\setup.ps1` to apply the overlay and verify the submodule

## Building

```powershell
cd Plugins/InoAgents/LiteRtLm
.\scripts\build-win64.ps1
```

First cold build is ~30–60 minutes. Subsequent incremental builds are seconds.

Outputs are staged directly into the plugin:

- `Plugins/InoAgents/Binaries/ThirdParty/InoAgentsLibrary/Win64/LiteRtLm.dll`
- `Plugins/InoAgents/Source/ThirdParty/InoAgentsLibrary/Win64/LiteRtLm.lib`
- `Plugins/InoAgents/Source/ThirdParty/InoAgentsLibrary/Public/litert/lm/engine.h`

## Updating LiteRT-LM

```powershell
.\scripts\update-litert.ps1 v0.11.0
```

This bumps the submodule, rewrites `LITERT_LM_TAG`, and rebuilds. Commit the
submodule pointer only after verifying the build succeeds and UE still loads
the DLL.

## Current target

The build produces `//ino:LiteRtLm` which is a `cc_binary(linkshared=1)`
depending on `//c:engine_cpu` (CPU backend only — smaller surface area for the
first build). Swap to `//c:engine` (full CPU + GPU) by editing
`overlay/ino/BUILD.bazel` and re-running the build script. The incremental
rebuild reuses cached dependencies.

## Do not edit files inside `vendor/LiteRT-LM/`

The submodule is upstream code. Our own customizations live in `overlay/` and
are copied into the submodule at setup time. The submodule's `.git/info/exclude`
is updated by `setup.ps1` to ignore our overlay files so the submodule working
tree stays clean from git's perspective.
