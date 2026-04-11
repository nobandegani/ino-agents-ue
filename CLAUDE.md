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
│   ├── vendor/LiteRT-LM/                          ← git submodule, upstream pinned at v0.10.1 (c7b77b5)
│   ├── overlay/                                   ← files staged into the submodule before each build
│   │   └── ino/
│   │       ├── BUILD.bazel                        ← //ino:LiteRtLm target, deps //c:engine_cpu
│   │       └── LiteRtLm_exports.cc                ← force-reference stub (see "Custom Bazel target")
│   ├── scripts/                                   ← setup.ps1, build-win64.ps1, update-litert.ps1, clean.ps1
│   ├── LITERT_LM_TAG
│   └── README.md
├── Source/
│   ├── InoAgents/                                 ← runtime module (UE-facing, wraps the C API)
│   └── ThirdParty/InoAgentsLibrary/               ← External module consuming the built artifacts
│       ├── Public/litert/lm/engine.h              ← staged header (copy of vendor/LiteRT-LM/c/engine.h)
│       └── Win64/LiteRtLm.lib                     ← staged import library (~108 KB)
└── Binaries/ThirdParty/InoAgentsLibrary/Win64/
    ├── LiteRtLm.dll                               ← main runtime DLL (~17 MB, gitignored)
    └── libGemmaModelConstraintProvider.dll        ← required sibling runtime DLL (~13 MB, gitignored)
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
- **Windows static-library export quirk (IMPORTANT).** Upstream `build:windows --legacy_whole_archive=0` disables `--whole-archive` on Windows (upstream bug `b/469455895`). This has a non-obvious consequence: when our `cc_binary(linkshared=1)` target depends on a `cc_library` like `//c:engine_cpu`, **MSVC's linker only pulls in `.obj` files from that static library that are referenced by already-included code**. A `__declspec(dllexport)` annotation on a function in an otherwise-unreferenced `.obj` is silently dropped — the build succeeds, but the function never reaches the DLL's export table. See "Custom Bazel target" for how we work around this with a force-reference stub.

## Custom Bazel target

We define exactly one new Bazel target via the overlay, in a new package inside the submodule:

- **`//ino:LiteRtLm`** — a `cc_binary(linkshared=1, linkstatic=1)` that depends on `//c:engine_cpu` (start with CPU-only; swap to `//c:engine` when GPU support is added in a later iteration)
- Built output: `LiteRtLm.dll` (automatic Windows naming from `cc_binary(linkshared=1)`)
- Reuses `build:windows --config=monolithic` from upstream `.bazelrc` — all transitive deps link statically into the single DLL
- **We do NOT use a `/DEF:` file** (unlike upstream's `runtime/engine:litert_lm_main`). `/DEF:` is *exclusive* — it overrides `__declspec(dllexport)` and the linker's `/OPT:REF` dead-strips anything not listed. Trying it produced a 12 MB DLL with zero `litert_lm_*` exports.

### Why `LiteRtLm_exports.cc` exists

Bazel's `cc_binary` rule requires at least one source file. Ours, `overlay/ino/LiteRtLm_exports.cc`, serves two roles:

1. **DllMain stub** (standard Windows DLL entry point).
2. **Force-reference of every `litert_lm_*` C API function** — a `volatile` array of function pointers that takes the address of each exported API function. Because `LiteRtLm_exports.cc` is part of the `cc_binary`'s own `srcs` (not a static library), its `.obj` is always linked. Taking the address of each function creates hard link-time references, forcing MSVC to pull in the `.obj` files from `//c:engine_cpu`'s static archive. Once those `.obj` files are pulled in, the `__declspec(dllexport)` annotations on their symbols (via `LITERT_LM_C_API_EXPORT` in `c/engine.h`) drive the export table.

**Maintenance:** the force-reference array must be kept in sync with the functions declared in `c/engine.h`. When LiteRT-LM is upgraded, re-extract the current list with:

```bash
awk '/^LITERT_LM_C_API_EXPORT$/{flag=1; next} flag{
     match($0, /litert_lm_[a-zA-Z_0-9]+/);
     print substr($0, RSTART, RLENGTH); flag=0}' \
     vendor/LiteRT-LM/c/engine.h
```

and reconcile against `LiteRtLm_exports.cc`. A missing entry results in the corresponding symbol silently dropping from `LiteRtLm.dll` — the build succeeds, but callers fail at UE link time with "unresolved external symbol".

### Upstream bugs worked around in BUILD.bazel

- **`litert_lm_set_min_log_level`**: upstream `c/litert_lm_logging.h` declares this function without `__declspec(dllexport)`, while `c/engine.h` declares the *same function* with it. Since `litert_lm_logging.cc` includes only the non-exporting header, its `.obj` is compiled without the export marker, and force-reference alone is insufficient. Work around with an additive `/EXPORT:litert_lm_set_min_log_level` linkopt in our BUILD.bazel. File upstream issue and remove the linkopt when fixed.

### `libGemmaModelConstraintProvider.dll`

Our `LiteRtLm.dll` depends on `libGemmaModelConstraintProvider.dll` at runtime. This is an upstream **prebuilt** (LFS-tracked) binary at `vendor/LiteRT-LM/prebuilt/windows_x86_64/libGemmaModelConstraintProvider.dll`, ~13 MB. Some `cc_library` target in the `//c:engine_cpu` dep graph declares it as a data dependency, and Bazel symlinks it into `bazel-bin/ino/` alongside our DLL. `build-win64.ps1` copies the symlink target (the real file) into `Binaries/ThirdParty/InoAgentsLibrary/Win64/`. `InoAgentsLibrary.Build.cs` must list it in `PublicDelayLoadDLLs` and `RuntimeDependencies` so UE stages it alongside the executable. Without it, `LiteRtLm.dll` fails to load at runtime.

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
| **Windows long paths enabled** | `HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`. Necessary but NOT sufficient — `link.exe` does not transparently use the `\\?\` prefix for its input files, so some tools in the build still hit MAX_PATH even with long paths on. The short `--output_base` below is the actual fix. |
| **40+ GB free on the Bazel output drive** | First cold build of LiteRT-LM + all transitive deps is ~20 GB of caches + outputs. |
| **Short Bazel `--output_base`** (MANDATORY, not optional) | `scripts/build-win64.ps1` passes `bazelisk --output_base=C:/b/ino build ...`. Matches upstream CI's pattern (`D:/w-<hash>/`) from `.github/workflows/ci-build-win.yml`. Without this, intermediate filenames like `…/crate_index__macro_rules_attribute-proc_macro-0.2.2/…cgu.0.rcgu.o` exceed 260 chars and `link.exe` fails with `LNK1181: cannot open input file`. `setup.ps1` creates `C:/b/ino` on first run — no admin needed on a modern Windows 10/11 user profile. |
| **Antivirus exclusion for `C:\b\`** | Real-time scanning of Bazel's output root slows cold builds 3–5x. Not mandatory but strongly recommended. |

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

- **Runtime DLLs to ship alongside the executable.** Two files must end up in `Binaries/ThirdParty/InoAgentsLibrary/Win64/`:
  - `LiteRtLm.dll` (~17 MB) — our monolithic output
  - `libGemmaModelConstraintProvider.dll` (~13 MB) — upstream prebuilt, required sibling. See "Custom Bazel target → libGemmaModelConstraintProvider.dll".
  Both are handled by `build-win64.ps1` on the Bazel side and by `InoAgentsLibrary.Build.cs` on the UE side (both listed in `PublicDelayLoadDLLs` and `RuntimeDependencies`).
- **DXC runtime DLLs (conditional).** If/when we enable GPU inference by swapping the BUILD target to `//c:engine`, LiteRT-LM's D3D12 shader compilation at runtime may require `dxil.dll` + `dxcompiler.dll` from Microsoft's [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) releases. These are **not** Bazel outputs. With the current CPU-only build (`//c:engine_cpu`) they are not needed and not shipped. Revisit when switching to GPU.
- **Delay-load the DLLs.** `InoAgentsLibrary.Build.cs` uses `PublicDelayLoadDLLs.Add(...)` for both runtime DLLs so the game / editor launches even if they're missing. `FInoAgentsModule::StartupModule` calls `FPlatformProcess::GetDllHandle` explicitly and surfaces a `UE_LOG` error on failure — **no `FMessageDialog` fallback.** The original plugin template showed a blocking dialog on missing DLL; that dialog must be removed.
- **Monolithic DLL.** Upstream `build:windows --config=monolithic` links all transitive dependencies (absl, protobuf, tensorflow lite, xnnpack, tokenizers_cpp, llguidance, minja, miniaudio, sentencepiece, etc.) statically into `LiteRtLm.dll`. We do not ship separate dependency DLLs except for `libGemmaModelConstraintProvider.dll`, which is a prebuilt upstream binary outside the Bazel graph.
- **MSVC runtime.** Build with `/MD` (dynamic CRT) to match UE. `/MT` would link successfully but produce two CRTs in the same process at runtime, causing silent heap corruption across allocator boundaries. Upstream `build:windows` already handles this correctly — no explicit override needed in our overlay.
- **Force-reference the C API symbols.** See "Custom Bazel target → Why `LiteRtLm_exports.cc` exists". Without this, the DLL builds but exports no `litert_lm_*` functions because MSVC drops unreferenced `.obj` files from static libraries, and upstream disables `--whole-archive` on Windows.

## Model file distribution

Gemma 4 `.litertlm` model files are 2.5–5 GB and **must never be committed**. They are also not redistributed with the plugin — developers download the model files they need manually.

### Dev-time location (inside the plugin)

```
Plugins/InoAgents/
└── Models/
    └── gemma-4-E2B-it.litertlm          ← 2.58 GB, developer-downloaded, gitignored
```

**Why inside the plugin and not in the host project:** the plugin is the primary artifact; the demo project exists only to exercise the plugin. Models travel with the plugin so that the plugin is self-contained when someone consumes it. The plugin's `.gitignore` excludes `Models/` so the 2.5 GB file can never land in git.

**How the plugin resolves the model path at runtime:**

```cpp
const FString BaseDir = IPluginManager::Get().FindPlugin(TEXT("InoAgents"))->GetBaseDir();
const FString ModelPath = FPaths::Combine(BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));
```

This is the same `IPluginManager` pattern used in `FInoAgentsModule::StartupModule` for locating the native DLLs. One consistent convention: *anything the plugin needs to find at runtime lives under the plugin's base directory and is located via `IPluginManager::FindPlugin`*.

### Phase 1 smoke test

Hardcoded path (`Models/gemma-4-E2B-it.litertlm`) is acceptable for the phase-1 bring-up milestone. A later milestone replaces the hardcode with a `UInoAgentsModelConfig` data asset so designers can swap models per demo / difficulty / level without code changes.

### Shipping builds (not phase 1)

For shipping, the model can't live inside the plugin tree — it would bloat the packaged build. The plan for shipping, which we will implement in a later phase:
- **Download on first run** to `FPaths::ProjectPersistentDownloadDir()` (canonical UE location for runtime-acquired user content)
- Verify SHA-256 against a manifest baked into the game
- Show a progress UI (the download is 2.5–5 GB)

### Model sources

Hugging Face, Apache 2.0, public (no gating, no auth):
- `litert-community/gemma-4-E2B-it-litert-lm` — 2.58 GB, Text + Image
- `litert-community/gemma-4-E4B-it-litert-lm` — ~5 GB, Text + Image + Audio

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
