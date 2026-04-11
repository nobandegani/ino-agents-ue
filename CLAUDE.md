# CLAUDE.md — InoAgents plugin

This file provides guidance to Claude Code (claude.ai/code) when working inside `Plugins/InoAgents/`. The hosting demo project is documented in `E:/Projects/InoAgentDemo/CLAUDE.md`.

## Purpose

`InoAgents` is an Unreal Engine 5.7 runtime plugin that embeds **Google Gemma 4** on-device, so UE games and tools can run LLM-powered agents inside the game process with no external server and no cloud dependency.

The plugin is named for "agents" deliberately: the long-term goal is not just text generation but tool-use / function-calling workflows running natively in UE.

## Runtime

The plugin uses **LiteRT-LM** (Google's open-source on-device LLM runtime — the successor to the deprecated MediaPipe LLM Inference API) as its single inference backend.

- Repo: https://github.com/google-ai-edge/LiteRT-LM
- Docs: https://ai.google.dev/edge/litert-lm

Why LiteRT-LM and not llama.cpp, ONNX Runtime GenAI, MLX, or MediaPipe:

1. **Single runtime covers every target platform in this plugin's roadmap.** LiteRT-LM has stable native C++ support for Windows, Linux, macOS, Android, and iOS. llama.cpp is great on desktop but painful to integrate into UE's Android NDK toolchain; MediaPipe LLM Inference API is deprecated on Android/iOS and has no desktop story; MLX is Mac-only.
2. **Google's first-class runtime for Gemma 4.** Prebuilt model bundles exist at `litert-community/gemma-4-E2B-it-litert-lm` and `litert-community/gemma-4-E4B-it-litert-lm` on Hugging Face.
3. **Native tool-use / function-calling is a framework feature**, not something to prompt-engineer on top of a raw completion API. This matches the plugin's "agents" mandate.
4. **GPU path on Windows uses DirectX (DXC / D3D12)**, which is the same RHI the demo project already runs on (`DefaultGraphicsRHI=DefaultGraphicsRHI_DX12` in `Config/DefaultEngine.ini`). No CUDA / Vulkan / OpenCL runtime to ship alongside the game.
5. **Native Win64 support is verified** — the v0.10.1 release ships `litert_lm_main.windows_x86_64.exe` and `libLiteRtWebGpuAccelerator.dll` as CI-built artifacts. The README's "Windows (WSL)" phrasing refers to the easiest way to try the CLI, not a technical limitation. Build toolchain is MSVC (Visual Studio 2022) with `bazelisk build ... --config=windows`.

Known caveat: LiteRT-LM is still **pre-1.0** (v0.10.x at time of this decision). Expect upstream API churn — pin to a specific tag, don't track `main`.

## Model scope

Only the two "E" (edge / on-device) Gemma 4 variants are in scope for this plugin:

| Variant | Effective params | Context | Modalities | Memory (Q4_0) |
|---|---|---|---|---|
| **E2B** | ~2B | 128K | Text + Image | ~3.2 GB |
| **E4B** | ~4B | 128K | Text + Image + **Audio** | ~5 GB |

The Gemma 4 **31B dense** and **26B A4B MoE** server-class variants are **intentionally out of scope** — they are not realistic to run inside a consumer UE game process alongside a renderer (17+ GB VRAM just for weights), and LiteRT-LM is an edge runtime, not a server runtime.

## Phase plan

The plugin is delivered in five platform phases. Every phase ships the same C++ API; only the underlying LiteRT-LM build + staged binaries differ.

| Phase | Platform | Scope notes |
|---|---|---|
| **1** | **Windows (native Win64, MSVC)** | **Text-only**. Gemma 4 E2B first; E4B after text path is stable. No image/audio in phase 1 — skip `mtmd`, vision projector, audio codec. D3D12/DXC GPU path. |
| 2 | Android | Port the Build.cs branch. LiteRT-LM native Android, not WSL/emulator. |
| 3 | iOS | Port the Build.cs branch. LiteRT-LM native iOS. |
| 4 | Linux | Port the Build.cs branch. |
| 5 | macOS | Port the Build.cs branch. |

Phases 2–5 are platform ports of the same plugin code. No backend abstraction layer is needed — LiteRT-LM is the one backend, and its C++ API is the same across platforms.

Modality expansion (image → audio) happens **after** phase 1's text path is stable, not as part of any phase transition.

## Current state of this plugin (important)

As of now, **this plugin is still the stock UE "Third Party Library" plugin template** and has not yet been pointed at LiteRT-LM. Concretely:

- `Source/ThirdParty/InoAgentsLibrary/` is still Epic's `ExampleLibrary` scaffold (its own `ExampleLibrary.sln`, prebuilt `ExampleLibrary.lib` + `ExampleLibrary.dll`).
- `Source/InoAgents/Private/InoAgents.cpp` calls `FPlatformProcess::GetDllHandle` on `ExampleLibrary.dll`, invokes `ExampleLibraryFunction()`, and pops a blocking `FMessageDialog` on failure. **This dialog fires every time the editor starts if the DLL isn't staged.**
- `InoAgentsLibrary.Build.cs` is the standard UE external-module pattern: `PublicAdditionalLibraries.Add(...)` for the import lib, `PublicDelayLoadDLLs.Add(...)` for the runtime DLL, `RuntimeDependencies.Add(...)` for staging into `Binaries/ThirdParty/InoAgentsLibrary/Win64/`.

**The existing scaffold is a feature, not a bug** — it is exactly the pattern we want for linking LiteRT-LM. The phase 1 integration work is to swap `ExampleLibrary` for LiteRT-LM inside that same scaffold:

1. Rename / replace `Source/ThirdParty/InoAgentsLibrary/` with the LiteRT-LM headers (`Public/`) and the LiteRT-LM Windows import library.
2. Update `InoAgentsLibrary.Build.cs` so:
   - `PublicSystemIncludePaths` points at LiteRT-LM's `Public/` headers.
   - `PublicAdditionalLibraries.Add(...)` points at LiteRT-LM's Windows `.lib`.
   - `PublicDelayLoadDLLs.Add(...)` lists LiteRT-LM's runtime DLL.
   - `RuntimeDependencies.Add(...)` stages the runtime DLL **and** `dxil.dll` + `dxcompiler.dll` (required in the same directory as the executable for GPU-accelerated builds on Windows — see "Windows gotchas" below).
3. Rewrite `FInoAgentsModule::StartupModule()` to initialise LiteRT-LM lazily (do **not** block on model load in `StartupModule` — the editor will freeze for seconds). Remove the `FMessageDialog` fallback entirely, or replace it with a `UE_LOG(..., Error, ...)` call.
4. Remove the residual `ExampleLibrary` sources (`ExampleLibrary.sln`, `ExampleLibrary.cpp`, `ExampleLibrary.vcxproj*`, prebuilt `x64/Release/ExampleLibrary.*`) once the LiteRT-LM path is working end-to-end, so nothing in the tree still references the template.

## Windows gotchas (phase 1)

- **DXC runtime DLLs.** LiteRT-LM's Windows GPU path requires `dxil.dll` and `dxcompiler.dll` from the DirectX Shader Compiler to be present alongside the executable at runtime. These are not shipped by UE and are not part of LiteRT-LM itself — they must be downloaded from Microsoft's DirectXShaderCompiler releases, staged into `Plugins/InoAgents/Binaries/ThirdParty/InoAgentsLibrary/Win64/`, and declared as `RuntimeDependencies` in `InoAgentsLibrary.Build.cs`. Without them, GPU inference fails at runtime on end-user machines.
- **`StartupModule` must not block on model load.** The Gemma 4 E2B task bundle is ~3.2 GB at Q4_0; loading it synchronously in `StartupModule` freezes the editor. Load on first request, off the game thread, with a completion callback / Blueprint latent node.
- **Inference runs off the game thread.** LLM decoding is CPU/GPU-heavy and streaming; run it on a dedicated `FRunnable` or `UE::Tasks` pipeline and marshal token callbacks back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`. Never call inference from Tick.
- **Model bundle location.** Do not commit the `.task`-style Gemma 4 bundle to the repo (too large). Stage it under `Content/` via an asset pointer or fetch it on first run. Final decision on this is part of the phase 1 plan.

## What to verify before trusting this file

This file was written during a brainstorming session; by the time you're reading it, some things may have drifted. Before acting on any specific claim here:

- Re-check LiteRT-LM's current version and platform list at https://github.com/google-ai-edge/LiteRT-LM/releases — we pinned expectations against v0.10.1.
- Re-check Gemma 4 variant specs and modality support at https://ai.google.dev/gemma/docs/core.
- If this file still describes the plugin as "stock UE template scaffold," actually open `Source/InoAgents/Private/InoAgents.cpp` and `Source/ThirdParty/InoAgentsLibrary/InoAgentsLibrary.Build.cs` — by the time you read this, the LiteRT-LM swap may already be done and this section is stale.
