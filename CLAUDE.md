# CLAUDE.md — InoAgents plugin

This file provides guidance to Claude Code (claude.ai/code) when working inside `Plugins/InoAgents/`. The hosting demo project is documented in `E:/Projects/InoAgentDemo/CLAUDE.md`.

## Purpose

`InoAgents` is an Unreal Engine 5.7 runtime plugin that embeds **Google Gemma 4** on-device, so UE games and tools can run LLM-powered agents inside the game process with no external server and no cloud dependency.

The plugin is named for "agents" deliberately: the goal is not just text generation but **tool-use / function-calling workflows** running natively in UE, driven from Blueprint.

## Runtime choice: LiteRT-LM

The plugin uses **LiteRT-LM** (Google's open-source on-device LLM runtime — the successor to the deprecated MediaPipe LLM Inference API) as its single inference backend.

- Upstream: https://github.com/google-ai-edge/LiteRT-LM
- Docs: https://ai.google.dev/edge/litert-lm

Why LiteRT-LM and not llama.cpp, ONNX Runtime GenAI, MLX, or MediaPipe:

1. **Single runtime covers every target platform in this plugin's roadmap** — Windows, Linux, macOS, Android, iOS. llama.cpp is great on desktop but painful on UE's Android NDK toolchain. MediaPipe LLM Inference API is deprecated on Android/iOS and has no desktop story. MLX is Mac-only. ONNX Runtime GenAI has no first-class Gemma 4 support.
2. **Google's first-class runtime for Gemma 4.** Pre-built model bundles live at `litert-community/gemma-4-E2B-it-litert-lm` and `litert-community/gemma-4-E4B-it-litert-lm` on Hugging Face.
3. **Native tool-use / function-calling is a framework feature** surfaced through the public C API (`litert_lm_conversation_*`). Not prompt-engineered on top of a raw completion API. This matches the plugin's "agents" mandate.
4. **GPU path on Windows uses DirectX (DXC / D3D12)**, matching the demo project's `DefaultGraphicsRHI=DefaultGraphicsRHI_DX12`. No CUDA / Vulkan / OpenCL runtime to ship alongside the game.
5. **Native Win64 MSVC support is verified.** Upstream CI ships `litert_lm_main.windows_x86_64.exe` as a release artifact; the build toolchain is Bazel + MSVC + bazelisk. We do not depend on WSL for anything.

Known caveat: LiteRT-LM is **pre-1.0** (v0.10.x). Expect upstream API churn — we pin to a specific tag via submodule, never track `main`.

## Integration approach: link, not subprocess

We considered and rejected a subprocess-based integration (spawning `litert_lm_main --multi_turns` and piping stdin/stdout). Reasons:

1. **Tool calling is only available through the C++ / C API, not the CLI.** The CLI's `--multi_turns` mode does plain text chat only. A subprocess backend could never expose the plugin's headline feature.
2. **Subprocess is impossible on iOS** (code signing prohibits `exec` of bundled binaries) **and forbidden by Google Play Protect on Android.** Since phases 2–3 must link LiteRT-LM as a library anyway, doing subprocess for phase 1 would just mean writing the same backend twice.
3. **Crash isolation is real but restart cost is huge.** Reloading a 3.2 GB Gemma 4 model after a subprocess crash takes seconds. Not a win in practice.

The plugin uses **one integration strategy across all five platform phases: linked library via the LiteRT-LM public C API**.

## DLL boundary: pure C API (`c/engine.h`)

The LiteRT-LM public API at `LiteRtLm/vendor/LiteRT-LM/c/engine.h` is a **pure C API** wrapped in `extern "C"`, with `__declspec(dllexport)` already applied on Windows via `LITERT_LM_C_API_EXPORT`. LiteRT-LM was designed from day one to be a DLL boundary, which means the UE integration skips entire classes of pain:

- **No C++ ABI matching.** No libstdc++-vs-MSVC-STL concerns, no iterator ABI, no exception propagation across the DLL boundary, no RTTI worries.
- **No STL types in the boundary.** Opaque pointers (`LiteRtLmEngine*`, `LiteRtLmSession*`, `LiteRtLmConversation*`, etc.) + primitive types + C callbacks. Nothing more.
- **No hand-written wrapper layer.** `InoAgentsEngine.cpp` `#include`s `c/engine.h` and calls the C functions directly. UE-side work is marshalling between C types and UE types, not bridging ABIs.

The C API already covers what we need:

- Engine / session / conversation lifecycle with CPU or GPU backend selection
- Streaming generation with C callback (`LiteRtLmStreamCallback`)
- Multimodal input (text / image / audio via `InputData` struct)
- Conversation API with tool calling: `litert_lm_conversation_create` accepts `tools_json` + `messages_json` parameters, `send_message_stream` dispatches tool calls, `cancel_process` cancels inference, constrained decoding is a flag
- Benchmarking

## Model scope

Only the two "E" (edge / on-device) Gemma 4 variants are in scope for this plugin:

| Variant | Effective params | Context | Modalities | Memory (Q4_0) |
|---|---|---|---|---|
| **E2B** | ~2B | 128K | Text + Image | ~3.2 GB |
| **E4B** | ~4B | 128K | Text + Image + **Audio** | ~5 GB |

Gemma 4's **31B dense** and **26B A4B MoE** server-class variants are **intentionally out of scope** — they are not realistic to run inside a consumer UE game process alongside a renderer (17+ GB VRAM just for weights), and LiteRT-LM is an edge runtime, not a server runtime.

## Repository layout

```
Plugins/InoAgents/
├── InoAgents.uplugin
├── LiteRtLm/                                      ← Bazel build workspace (self-contained)
│   ├── vendor/LiteRT-LM/                          ← git submodule, upstream pinned at v0.10.1
│   ├── overlay/                                   ← files staged into the submodule before each build
│   ├── scripts/                                   ← setup.ps1, build-win64.ps1, update-litert.ps1, clean.ps1
│   ├── .bazelversion, .bazelrc, LITERT_LM_TAG
│   └── README.md
├── Source/
│   ├── InoAgents/                                 ← runtime module (UE-facing, wraps the C API)
│   └── ThirdParty/InoAgentsLibrary/               ← External module consuming the built artifacts
│       ├── Public/litert/lm/                      ← staged headers (c/engine.h copy)
│       └── Win64/LiteRtLm.lib                     ← staged import library
└── Binaries/ThirdParty/InoAgentsLibrary/Win64/
    └── LiteRtLm.dll                               ← staged runtime DLL (produced by build script; gitignored)
```

**`LiteRtLm/vendor/LiteRT-LM/`** is a git submodule (`https://github.com/google-ai-edge/LiteRT-LM.git`) pinned at tag **v0.10.1** (commit `c7b77b5`). We never edit files inside the submodule directly. Version bumps happen via `LiteRtLm/scripts/update-litert.ps1`, which updates the submodule pointer and re-runs the overlay + build.

**`LiteRtLm/overlay/`** holds files that need to land inside the submodule's source tree at build time (for example, a custom `BUILD.bazel` target that produces our DLL). The overlay is tracked in the plugin repo. `setup.ps1` copies overlay files into the submodule and adds them to the submodule's `.git/info/exclude` so the submodule working tree stays clean from git's perspective.

**`Source/ThirdParty/InoAgentsLibrary/`** is the existing UE `External` module, extended to consume LiteRT-LM. The stock `ExampleLibrary` scaffold that shipped with the plugin template is the starting pattern — `PublicAdditionalLibraries.Add(...)` for the import lib, `PublicDelayLoadDLLs.Add(...)` for the runtime DLL, `RuntimeDependencies.Add(...)` for staging into `Binaries/ThirdParty/...`. The `Build.cs` is rewritten to point at LiteRT-LM's artifacts instead of `ExampleLibrary`'s.

## Build system: Bazel 7.6.1 via bazelisk

LiteRT-LM is built from source with Bazel. We do not use the prebuilt artifacts from upstream GitHub releases — we control the build ourselves so we can produce a `cc_binary(linkshared=1)` DLL target that isn't in their shipped targets.

Key facts about the upstream Bazel setup:

- **Pinned Bazel version: 7.6.1** (`vendor/LiteRT-LM/.bazelversion`). Bazelisk auto-fetches this on first build.
- **Legacy `WORKSPACE` mode** (`common --noenable_bzlmod`). We do not try to use bzlmod.
- **Upstream `.bazelrc` has a `build:windows` config** that we inherit. Key flags it already sets for us:
  - `--config=monolithic` — on Windows, all transitive deps link statically into a single shared object. This is exactly what we want — one `LiteRtLm.dll` with everything inside.
  - `--cxxopt=/std:c++20` — C++20 mode
  - `--copt=/arch:AVX2` — AVX2 baseline (any x86_64 CPU since ~2013)
  - `--copt=/DLITERT_DISABLE_OPENCL_SUPPORT=1` — OpenCL disabled (we use D3D12 anyway)
  - `--shell_executable="C:/Program Files/Git/bin/bash.exe"` — **upstream hardcodes Git at this exact path** for shell genrules
  - `startup --windows_enable_symlinks` + `--enable_runfiles` — requires Developer Mode enabled
- **Windows symbol export pattern** (from `runtime/engine/BUILD:180` and `c/engine.h`): two layers acting together:
  1. `c/engine.h` annotates the `litert_lm_*` C functions with `__declspec(dllexport)` (compile-time)
  2. `runtime/engine:litert_lm_main` attaches `@litert//litert/c:windows_exported_symbols.def` as a linker input via `/DEF:$(location ...)` linkopt (link-time, for the internal `LiteRt*` accelerator ABI)
  Our DLL target reuses both mechanisms verbatim.

## Custom Bazel target

We define exactly one new Bazel target via the overlay, in a new package inside the submodule:

- **`//ino:LiteRtLm.dll`** — a `cc_binary(name=..., linkshared=1)` that depends on `//c:engine` (the full public engine library: CPU + GPU backends, multimodal, tool calling)
- Reuses `windows_exported_symbols.def` linker input + linkopt exactly as `runtime/engine:litert_lm_main` does
- Reuses `build:windows --config=monolithic` so all transitive deps land in the single DLL
- Alternate CPU-only dependency available: `//c:engine_cpu` (smaller surface area, smaller transitive dep graph — useful for first-build bring-up, can be swapped to `//c:engine` once the end-to-end path works)

## Toolchain requirements (Windows host)

A developer machine needs all of the following before `scripts/build-win64.ps1` can succeed:

| Requirement | How |
|---|---|
| **Developer Mode enabled** | Settings → System → For developers → Developer Mode → On. Bazel needs symlink-creation rights. |
| **Visual Studio 2022** with the C++ workload | Community edition is fine. MSVC toolset 14.38+ required. |
| **`BAZEL_VC` user env var** | Set to `<VS install>\VC` — e.g. `C:\Program Files\Microsoft Visual Studio\2022\Community\VC`. Single backslashes (Windows env var UI takes raw strings, no escaping). |
| **Bazelisk on PATH** | `winget install Bazel.Bazelisk`. Bazelisk auto-downloads the Bazel version pinned by `.bazelversion`. |
| **Git at `C:\Program Files\Git`** | Required because upstream `.bazelrc` hardcodes `C:/Program Files/Git/bin/bash.exe` for shell genrules. |
| **Python 3 on PATH** | Used by Bazel's protobuf / XNNPACK build rules. Any 3.10+ works. |
| **Windows long paths enabled** | `HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`. Bazel builds create deeply nested output paths that overflow MAX_PATH without this. |
| **40+ GB free on the Bazel output drive** | First cold build of LiteRT-LM + all transitive deps is enormous. |
| **Bazel output root on a short path** | Recommend `startup --output_user_root=C:/b` in `LiteRtLm/.bazelrc` — further mitigates MAX_PATH issues and speeds up Windows Defender scans by staying in one predictable location. |
| **Antivirus exclusion for Bazel output root** | Real-time scanning of `C:\b\...` slows cold builds 3–5x. Not mandatory but strongly recommended. |

The preflight check for all of this lives in `LiteRtLm/scripts/setup.ps1` and should be the first thing a new dev runs.

## UE-side integration architecture

```
Blueprint ─┬─ UInoAgentsSubsystem    (UGameInstanceSubsystem)
           │     owns LiteRtLmEngine*, tool registry, async LoadModel
           │
           ├─ UInoAgentsSession       (UObject, BlueprintType)
           │     owns LiteRtLmSession* / LiteRtLmConversation*
           │     latent Generate node, OnToken, OnComplete, OnToolCallRequested delegates
           │
           └─ IInoAgentsTool          (Blueprint interface)
                 Name, SchemaJson, Execute(ArgsJson) → ResultJson
                    │
                    ▼
          FInoAgentsInferenceWorker   (FRunnable, one per session)
                 calls litert_lm_session_generate_content_stream()
                 static C callback marshals tokens → game thread via AsyncTask
                    │
                    ▼
                 LiteRtLm.dll  (pure C API)
```

### Threading model (non-negotiable)

- **`StartupModule` never blocks on model load.** Gemma 4 E2B is ~3.2 GB; synchronous load would freeze the editor for 5–30 seconds. `LoadModel` is an async Blueprint-callable that spawns a worker, calls `litert_lm_engine_create`, and fires a completion delegate back to the game thread.
- **Inference never runs on the game thread.** Every `UInoAgentsSession` owns an `FInoAgentsInferenceWorker` (an `FRunnable` on a dedicated `FRunnableThread`). Prompts enter the worker via a `TQueue<FInferenceCommand, EQueueMode::Mpsc>`. The worker calls into `litert_lm_session_generate_content_stream` with a static C callback.
- **Tokens marshal back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.** The static C callback never touches UObjects directly — it captures the session and schedules a game-thread task that broadcasts `OnToken`.
- **One worker per session.** LiteRT-LM sessions are stateful (KV cache) and not thread-safe. Concurrent conversations = multiple sessions, each with its own pinned worker thread.
- **Never call inference from `Tick`.** Not even once.

### Tool calling flow

1. Blueprint class implements `IInoAgentsTool` with `Name`, `SchemaJson` (OpenAPI-ish JSON schema), and `Execute(FString ArgsJson) → FString ResultJson`.
2. `UInoAgentsSubsystem::RegisterTool` stores the tool by name and appends its schema to the `tools_json` used on the next `litert_lm_conversation_create`.
3. When the model emits a tool call, the LiteRT-LM C callback fires on the worker thread. The static callback dispatches a game-thread task to invoke the Blueprint tool's `Execute`.
4. The worker blocks on a `TPromise<FString>`; the game-thread task fulfils the `TFuture` once `Execute` returns.
5. The returned JSON result flows back into LiteRT-LM via the conversation API, and generation resumes.

Sync tools run on the game thread. Async tools that need to do their own async work block the worker's future until the game-thread work completes. The game thread itself never blocks.

**Deadlock guard:** a tool that tries to call `Generate` on the same session from within its `Execute` implementation will deadlock on the worker queue. `UInoAgentsSession::Generate` must raise an error if called during tool execution.

## Phase plan

Five platform phases. Every phase ships the same C++ API and the same UE-side integration — only the `InoAgentsLibrary.Build.cs` branch and the Bazel build config differ.

| Phase | Platform | Notes |
|---|---|---|
| **1** | **Windows (Win64, MSVC)** | Full feature set (text / image / audio / tool calling) — the C API exposes everything from day one, no point in artificially restricting phase 1 to text-only. D3D12/DXC GPU path. |
| 2 | Android | `--config=android_arm64` already exists in upstream `.bazelrc`. Port `InoAgentsLibrary.Build.cs` with an `Android` branch. |
| 3 | iOS | `--config=ios_arm64` already exists in upstream `.bazelrc`. Port Build.cs with an `IOS` branch. |
| 4 | Linux | Port Build.cs with a `Linux` branch. |
| 5 | macOS | `build:macos_arm64` already exists. Port Build.cs with a `Mac` branch. |

No backend abstraction layer. LiteRT-LM is the one backend, and its public C API is identical on every platform.

## Windows gotchas

- **DXC runtime DLLs.** On Windows GPU path, D3D12 shader compilation at runtime needs `dxil.dll` + `dxcompiler.dll` from Microsoft's [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) releases. These are **not** Bazel outputs — they're prebuilt binaries we download separately via `scripts/fetch-dxc.ps1` and stage into `Binaries/ThirdParty/InoAgentsLibrary/Win64/` as `RuntimeDependencies`. Whether we actually need them depends on which GPU backend LiteRT-LM ends up using at runtime; we'll know once the first end-to-end run is working. If we stay on CPU backend, they're irrelevant.
- **Delay-load the DLL.** `InoAgentsLibrary.Build.cs` uses `PublicDelayLoadDLLs.Add("LiteRtLm.dll")` so the game / editor launches even if the DLL is missing. `FInoAgentsModule::StartupModule` calls `FPlatformProcess::GetDllHandle` explicitly and surfaces a `UE_LOG` error on failure — **no `FMessageDialog` fallback.** The original plugin template showed a blocking dialog on missing DLL; that dialog is removed.
- **Monolithic DLL.** Upstream `build:windows --config=monolithic` links all transitive dependencies (absl, protobuf, tensorflow lite, xnnpack, tokenizers_cpp, llguidance, minja, miniaudio, sentencepiece, etc.) statically into `LiteRtLm.dll`. We do not ship separate dependency DLLs.
- **MSVC runtime.** Build with `/MD` (dynamic CRT) to match UE. `/MT` would link successfully but produce two CRTs in the same process at runtime, causing silent heap corruption across allocator boundaries. This is pinned in our `LiteRtLm/.bazelrc`.

## Model file distribution

Gemma 4 E2B `.litertlm` model is ~3.2 GB at Q4_0. **Never commit to the repo.** Never ship via `RuntimeDependencies`. Options:

- **Dev / editor:** sideload from a configured path (`UInoAgentsModelConfig` data asset pointing at a local `.litertlm` file)
- **Shipping builds:** download on first run to `FPaths::ProjectPersistentDownloadDir()`, verify SHA-256 against a manifest baked into the game, show a progress UI

Model sources are Hugging Face: `litert-community/gemma-4-E2B-it-litert-lm`, `litert-community/gemma-4-E4B-it-litert-lm`.

Model configuration lives in a `UDataAsset` subclass so designers can swap models per level / difficulty / demo without code changes.

## How to update LiteRT-LM

```
cd Plugins/InoAgents/LiteRtLm/scripts
./update-litert.ps1 v0.11.0     # or any tag
```

The script:
1. `cd vendor/LiteRT-LM && git fetch --tags && git checkout <tag>`
2. Updates `LITERT_LM_TAG` file
3. Re-runs overlay application (`setup.ps1`)
4. Re-runs `build-win64.ps1`
5. Reports the new SHA for you to commit

**Never** stage the submodule pointer bump without re-running the build to verify upstream still compiles. LiteRT-LM is pre-1.0; breaking changes between tags are possible.

## What to verify before trusting this file

This file describes design decisions and architectural intent. Specifics drift over time. Before acting on any specific claim:

- **Upstream version:** check `LiteRtLm/vendor/LiteRT-LM/.bazelversion` and `git -C LiteRtLm/vendor/LiteRT-LM describe --tags` for the actual pinned version.
- **Actual Bazel target names:** read `LiteRtLm/vendor/LiteRT-LM/c/BUILD` and `runtime/engine/BUILD` — target names may have moved between versions.
- **The public C API:** read `LiteRtLm/vendor/LiteRT-LM/c/engine.h` directly. If the symbol names or signatures differ from what this file describes, trust the header.
- **Plugin scaffold state:** open `Source/ThirdParty/InoAgentsLibrary/InoAgentsLibrary.Build.cs` and `Source/InoAgents/Private/InoAgents.cpp`. If they still reference `ExampleLibrary`, the LiteRT-LM swap is not complete yet. If they reference `LiteRtLm`, it is.
- **Tooling versions:** Gemma 4 variant specs and modality support may have evolved — confirm against https://ai.google.dev/gemma/docs/core.
