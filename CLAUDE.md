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

### Known LiteRT-LM v0.10.1 runtime limitations

Two C API features are declared in the header and compile but fail at runtime with Gemma 4 models. Both are disabled in our code (with `#if 0` / default values) and marked with `TODO(litert-upgrade)` comments:

1. **Session config (sampler params + max output tokens).** Passing a non-null `LiteRtLmSessionConfig*` to `litert_lm_conversation_config_create` causes `litert_lm_conversation_create` to return NULL for Gemma 4 models. The C API code itself handles session config cleanly (upstream tests pass with small test models), so the failure is likely Gemma 4-specific — probably inside `SessionConfig::MaybeUpdateAndValidate` when it reconciles user-supplied sampler params against Gemma 4 metadata. A failed attempt also appeared to poison subsequent conversation creations on the same engine (producing error 13 on `send_message_stream`), though the source code does not obviously explain this side-effect. **Workaround:** pass `nullptr` for session config (uses engine defaults for all sampling). The `FLiteRtLmSamplerConfig` struct and `MaxOutputTokens` field exist in `FLiteRtLmModelConfig` for forward-compatibility but are not applied at runtime.

2. **Activation data type (F16/I16/I8).** `litert_lm_engine_settings_set_activation_data_type` with non-F32 values loads the engine successfully (model file loads, XNNPACK cache regenerates), but `litert_lm_conversation_send_message_stream` returns error 13 (`absl::StatusCode::kInternal`) at runtime. This is corroborated by the upstream source: `engine.cc:332-338` force-overrides activation to F32 for GPU backends, and a TODO bug (`b/433590109`) acknowledges FP16 GPU incompatibilities. For CPU, the XNNPACK delegate configuration likely fails when model tensors don't match the requested activation format. **Workaround:** default `ActivationType` to `F32`. The enum and field exist in `FLiteRtLmModelConfig` for forward-compatibility but should not be changed from F32 until a future LiteRT-LM release fixes this. If a user has previously loaded a model with F16 and gets error 13, deleting the XNNPACK cache (next to the model file, or in the custom CacheDir) forces regeneration with F32.

Both limitations were discovered empirically during development. When upgrading LiteRT-LM, re-test these two features first — they are the most impactful unlocks (lower RAM via F16, creative control via temperature).

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
│   │   ├── InoAgents.Build.cs
│   │   ├── Public/
│   │   │   └── InoAgents.h                        ← module interface (FInoAgentsModule)
│   │   └── Private/
│   │       ├── InoAgents.cpp                      ← module lifecycle + DLL loading only (~140 lines)
│   │       ├── InoAgentsLog.h                     ← shared LogInoAgents category declaration
│   │       └── SmokeTests/                        ← phase-1 dev-time console commands
│   │           ├── InoAgentsSmokeTestCommon.{h,cpp} ← shared helpers (model path, JSON parsing)
│   │           ├── InoAgentsLoadEngineTest.cpp    ← InoAgents.LoadEngineTest
│   │           ├── InoAgentsGenerateTest.cpp      ← InoAgents.GenerateTest
│   │           ├── InoAgentsConversationTest.cpp  ← InoAgents.ConversationTest (milestone A)
│   │           ├── InoAgentsToolCallTest.cpp      ← InoAgents.ToolCallTest (milestone B)
│   │           └── InoAgentsStreamTest.cpp        ← InoAgents.StreamTest (milestone C)
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

The UE-facing API lives under `Source/InoAgents/Public/LiteRtLm/` and `Private/LiteRtLm/`. Names are prefixed `LiteRtLm` rather than `InoAgents` on purpose — future versions of this plugin will host multiple backends (OpenAI, Anthropic, llama.cpp) and each backend's classes live in their own subdirectory. Naming the classes after the backend from day one makes the boundary explicit.

```
Blueprint ─┬─ UInoAgentsLiteRtLmAgentComponent  (USceneComponent, all-in-one)
           │     THE primary entry point. Drop on actor, set ModelConfig +
           │     VoiceId, call SendMessage. Internally owns + wires:
           │       child UInoAgentsStreamingAudioComponent (3D audio)
           │       UInoAgentsLiteRtLmDialogueQueue (ordered TTS)
           │       ULiteRtLmConversation (LLM chat)
           │     Delegates (pass-through): OnModelLoaded, OnToken,
           │       OnSentence(RawText,CleanText), OnComplete, OnError,
           │       OnAudioFinished, OnDownloadProgress
           │     Config: ModelConfig (FLiteRtLmModelConfig struct),
           │       VoiceId, TtsRequestTemplate, PauseDurationMs
           │
           ├─ ULiteRtLmSubsystem       (UGameInstanceSubsystem)
           │     owns LiteRtLmEngine*, tool registry, ShowChatPanel/
           │     HideChatPanel. LoadModelAsync auto-downloads models
           │     from URLs configured in UInoAgentsSettings.
           │     OnDownloadProgress fires during download.
           │
           ├─ ULiteRtLmConversation    (UObject, BlueprintType)
           │     owns one native LiteRtLmConversation* plus a pinned
           │     worker thread. Multicast delegates:
           │       OnToken(Chunk)
           │       OnSentence(RawText, CleanText)  — per newline
           │       OnNewLine()                     — pause signal
           │       OnComplete(FullText)
           │       OnError(ErrorMessage)
           │       OnToolCalled(Name, ArgsJson, ResultJson)
           │
           ├─ FLiteRtLmModelConfig     (USTRUCT, BlueprintType)
           │     plain struct (NOT a UDataAsset). Fields:
           │       ModelFileName — resolved via LiteRtLmResolveModelPath:
           │         1. PersistentDownloadDir/InoAgents/Models/ (cached)
           │         2. Plugins/InoAgents/Models/ (legacy dev)
           │         3. auto-download from UInoAgentsSettings URL
           │       Backend (Cpu / Gpu), MaxNumTokens, SystemMessage
           │
           ├─ UInoAgentsSettings       (UDeveloperSettings)
           │     unified Project Settings page under Plugins → InoAgents.
           │     Two sections:
           │       ElevenLabs: ApiKey, BaseUrl, DefaultModelId, OutputFormat
           │       LiteRT-LM → Models: array of {DisplayName, FileName, URL}
           │
           ├─ ILiteRtLmTool            (Blueprintable UInterface)
           │
           ├─ UInoAgentsStreamingAudioComponent  (UAudioComponent subclass)
           │     plays PCM int16 / PCM float32 / MP3 bytes at runtime.
           │     FeedAudioBytes + FinalizeStream + PlayAudio + StopAndReset.
           │     Pre-buffer before Play (configurable PreBufferMs).
           │     MP3 decoded via bundled minimp3 (CC0, single-header).
           │
           ├─ UInoAgentsLiteRtLmDialogueQueue    (UObject)
           │     auto-binds to conversation OnSentence + OnNewLine.
           │     Dispatches ElevenLabs TTS in parallel per-sentence,
           │     plays audio back in strict order via the streaming
           │     audio component. Pause slots between lines.
           │
           ├─ UElevenLabsSubsystem     (UGameInstanceSubsystem)
           │     caches settings, anchors live HTTP actions, CancelAll
           │     on PIE end.
           │
           └─ UElevenLabsTextToDialogueStream  (UBlueprintAsyncActionBase)
                 latent Blueprint node for /v1/text-to-dialogue/stream.
                 OnAudioChunk / OnComplete / OnError.
                      │
                      ▼
          FLiteRtLmConversationWorker   (FRunnable, one per conversation)
                 Owns the native LiteRtLmConversation and ConversationConfig.
                 Multi-round agent loop: user msg → tool calls → tool
                 results → final text. Marshals via AsyncTask(GameThread).
                      │
                      ▼
                 LiteRtLm.dll  (pure C API — litert_lm_conversation_*)
```

### Threading model (non-negotiable)

- **`StartupModule` never blocks on model load.** Gemma 4 E2B is ~3.2 GB; synchronous load would freeze the editor for 5–30 seconds. `LoadModelAsync` dispatches via `Async(EAsyncExecution::ThreadPool, ...)`, calls `litert_lm_engine_create` there, and marshals the `FOnLiteRtLmModelLoaded` delegate back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`. A `TWeakObjectPtr<ULiteRtLmSubsystem>` guards against the subsystem being torn down while the load is in flight.
- **Inference never runs on the game thread.** Every `ULiteRtLmConversation` owns an `FLiteRtLmConversationWorker` (an `FRunnable` on a dedicated `FRunnableThread`). Messages enter the worker via a `TQueue<FString, EQueueMode::Spsc>` whose producer is `EnqueueMessage` on the game thread. The worker calls `litert_lm_conversation_send_message_stream` which itself is non-blocking — it returns immediately and fires the C callback from LiteRT-LM's own internal thread. The worker uses two `FEvent`s:
  - **QueueEvent** (auto-reset) — wakes the worker thread when a new message is enqueued or `Stop` is called.
  - **StreamEvent** (manual-reset) — signalled by the stream callback when a round reaches `is_final` or errors. The worker thread blocks on this inside `RunOneStreamRound` to serialise rounds within a send.
- **Tokens marshal back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.** The static C callback never touches `UObject` state directly — it copies `chunk` / `error_msg` into `FString`s (which own their storage), dispatches an `OnToken` broadcast via `AsyncTask`, and then signals `StreamEvent` for terminal callbacks. The worker thread's `ProcessMessage` eventually dispatches the terminal `OnComplete` / `OnError` via the same `AsyncTask` pattern, so observers always see `OnToken`s in order followed by exactly one terminal broadcast.
- **TUniquePtr<FLiteRtLmConversationWorker> ordering.** Because the worker is a forward-declared type in the public `ULiteRtLmConversation` header, UHT's generated `.gen.cpp` emits both the default constructor and the `FVTableHelper` hot-reload helper constructor inline. Both must be declared out-of-line in the header and defined in `LiteRtLmConversation.cpp` (where `LiteRtLmConversationWorker.h` is fully included) so the `TDefaultDelete<FLiteRtLmConversationWorker>` deleter instantiation lands in a TU with the complete type. Leaving any of them implicit produces C4150 "delete of pointer to incomplete type" — see the comments at the top of the class in `LiteRtLmConversation.h`.
- **One worker per conversation.** LiteRT-LM conversations are stateful (KV cache) and not thread-safe. Concurrent conversations mean multiple native conversations, each with its own pinned worker thread. LiteRT-LM also appears to reject creating a second native conversation on the same engine while a prior one is still alive, so tests that run back-to-back must call `Conversation->Shutdown()` (synchronous worker teardown) before constructing the next one. `CollectGarbage` from inside a delegate handler is NOT a valid substitute — parallel GC workers racing the in-flight delegate's write access trigger `FMRSWRecursiveAccessDetector` ensure fires (learned the hard way during D.3).
- **Never call inference from `Tick`.** Not even once.

### Tool calling flow

1. A Blueprint class or C++ class implements `ILiteRtLmTool` with `GetToolName()`, `GetToolSchemaJson()` (OpenAI-style function-call schema), and `Execute(FString ArgumentsJson) → FString ResultJson`.
2. `ULiteRtLmSubsystem::RegisterTool` validates the schema parses as JSON and that its `function.name` field matches `GetToolName()` before storing the tool in its internal `TMap<FName, TScriptInterface<ILiteRtLmTool>>`. Mismatched / unparseable schemas are rejected with a clear error log.
3. `ULiteRtLmSubsystem::CreateConversation` calls `BuildToolsJsonForConversation` which serialises every registered tool's schema into a JSON array via `FJsonSerializer::Serialize` with `TCondensedJsonPrintPolicy`. The array plus `enable_constrained_decoding=true` are passed to `litert_lm_conversation_config_create`. When no tools are registered, both are left at their defaults and the conversation behaves as a plain chat.
4. When the model emits a tool call, LiteRT-LM delivers the chunk to the static C callback as an **OpenAI-compatible** envelope with `tool_calls` at the **top level** of the assistant message (NOT as a `content[*]` part — this was a D.4b bug until we read `TryExtractFirstToolCall` to confirm the actual shape):
   ```json
   {"role":"assistant",
    "tool_calls":[
      {"type":"function",
       "function":{"name":"add_numbers","arguments":{"a":27,"b":15}}}]}
   ```
   `OnStreamChunk` walks `content[*]` for text parts AND the top-level `tool_calls[*]` independently, so a single chunk can in principle carry either or both.
5. Tool-call entries are re-serialised (arguments sub-object → compact JSON string) and queued into `StreamPendingToolCalls`. Text from tool-call rounds is suppressed — `OnToken` only sees tokens from the final text-producing round, never intermediate tool-call JSON.
6. When the round's `is_final` callback fires, the worker thread wakes from `StreamEvent->Wait`, sees non-empty `StreamPendingToolCalls`, and runs `ExecuteToolSynchronously` for each. That method:
   - Allocates a pooled `FEvent` (auto-reset).
   - Dispatches an `AsyncTask` to the game thread that looks up the tool via `Subsystem->FindTool(ToolName)`, calls `ILiteRtLmTool::Execute_Execute(ToolObj, ArgsJson)` inside a try/catch, and triggers the `FEvent`.
   - Blocks on the `FEvent`. The game thread is never blocked because the outer `SendMessageAsync` is already async.
   - Returns the result JSON string (or a `"\"ERROR: ...\""` literal if the tool was missing / the subsystem was GC'd / Execute threw).
7. The worker builds a single `{"role":"tool","content":[{"type":"tool_response",...}, ...]}` message bundling every executed tool's result, sends that via a fresh `litert_lm_conversation_send_message_stream` on the **same** native conversation (reusing the KV cache), and loops back to round N+1.
8. Eventually a round produces final text with no tool calls. The worker dispatches `OnComplete` with the accumulated text of **that round only** — callers never see intermediate tool-call rounds. `OnToolCalled` fires once per tool execution on the game thread, strictly before the terminal `OnComplete`, as a diagnostic.
9. A safety cap (`kMaxAgentLoopRounds = 8`) bounds the loop. Hitting it dispatches `OnError("Agent loop exceeded N rounds...")` rather than spinning forever.

**Why this is not a deadlock trap.** Tools are executed via AsyncTask on the game thread while the worker blocks on an `FEvent`. The game thread itself is not blocked — `SendMessageAsync` has already returned control to the caller, so the game thread is free to run ticks, process more AsyncTasks, and eventually execute the tool. The worker wakes up when the tool is done.

**Deferred tool results.** `ULiteRtLmConversation::SubmitDeferredToolResult` is declared in the public API but currently stubbed — it logs a warning and is a no-op. A future update will wire it through the worker's agent loop so tools that need to do their own async work (network, disk I/O, user confirmation dialogs) can unblock the worker with a fresh result later. The method exists in the header now so Blueprint consumers can wire it up ahead of the implementation landing.

## Smoke tests

Development-time console commands. Two groups: the Phase 1 group exercises the native C API directly (no UObjects), and the UE API group exercises the UE-facing API surface end-to-end through PIE. Both groups stay in the codebase so a regression in either layer can be diagnosed without the other being a suspect.

Both groups live in `Source/InoAgents/Private/SmokeTests/`, one file per command, and register themselves as `FAutoConsoleCommand` globals at file scope so they become available the moment the module's DLL loads.

Invoke from the editor's Output Log command input. UE API tests require **PIE** (the subsystem is a `UGameInstanceSubsystem`), Phase 1 tests do not.

### Phase 1 — native C API layer (no UE API)

| Command | What it proves | PIE? |
|---|---|---|
| `InoAgents.LoadEngineTest` | LiteRT-LM engine can be constructed and destroyed without crashing. | no |
| `InoAgents.GenerateTest [prompt]` | Raw text generation via `session_generate_content` (no chat template). | no |
| `InoAgents.ConversationTest [prompt]` | Chat-template API via `conversation_send_message` actually follows instructions. | no |
| `InoAgents.ToolCallTest [prompt]` | Full tool-calling agent loop at the raw C API layer: user prompt → model emits tool call → we execute inline → tool result → final answer. Uses a local `add_numbers(a,b)` helper directly, NOT the D.4 `ULiteRtLmAddNumbersTool`. | no |
| `InoAgents.StreamTest [prompt]` | Non-blocking streaming via `generate_content_stream` with worker→game-thread marshaling through `AsyncTask`. First non-blocking smoke test. | no |

All Phase 1 tests except `StreamTest` are synchronous (freeze the editor for 2–15 s). They resolve the default model at `Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm` via `InoAgentsSmokeTest::ResolveDefaultModelPath()` and call the LiteRT-LM C API directly — no UObjects, no subsystem, no conversations. Their purpose is to prove the native integration works independently of the UE API layer.

### UE-facing API

| Command | What it proves | PIE? |
|---|---|---|
| `InoAgents.LiteRtLm.SubsystemLoadTest` | `ULiteRtLmSubsystem::LoadModelAsync` dispatches to a ThreadPool worker, marshals `FOnLiteRtLmModelLoaded` back to the game thread, and `IsModelLoaded` reports true afterward. Non-blocking. | **yes** |
| `InoAgents.LiteRtLm.ConversationSendTest` | `ULiteRtLmConversation` round-trips a non-streaming "What is 2 plus 2?" prompt through the worker's agent loop (streaming internally) and delivers the full accumulated text via `OnComplete`. Validates D.2 regression against D.3 streaming internals. | **yes** |
| `InoAgents.LiteRtLm.ConversationStreamTest [prompt]` | D.3 streaming surface: binds `OnToken` in addition to `OnComplete` and logs each chunk with per-stream elapsed time. Cross-checks that the locally-accumulated tokens match the `FullText` delivered to `OnComplete`. | **yes** |
| `InoAgents.LiteRtLm.ToolRegistryTest` | Registry-only check (no model load): constructs a `ULiteRtLmAddNumbersTool`, registers it, looks it up, serialises `BuildToolsJsonForConversation`, invokes `Execute_Execute` via the BlueprintNativeEvent wrapper, unregisters, and verifies `FindTool` returns null. Fastest D.4 smoke test; useful as a pre-flight before running the full agent loop. | **yes** |
| `InoAgents.LiteRtLm.ConversationToolTest [prompt]` | **The headline test.** Registers a `ULiteRtLmAddNumbersTool`, creates a conversation with `tools_json` + constrained decoding, binds all four delegates (`OnToken` / `OnToolCalled` / `OnComplete` / `OnError`), sends "What is 27 plus 15?", watches the multi-round agent loop run, and logs PASS if `OnToolCalled` fired with `add_numbers` + result `"42"` AND `OnComplete`'s text contains `"42"` or `"forty-two"`. | **yes** |

Every Milestone D observer UCLASS uses the same pattern: `NewObject` + `AddToRoot`, bind dynamic delegates via `AddDynamic`, run the workflow, and in `Finish()` call `Conversation->Shutdown()` for deterministic teardown before clearing UPROPERTY refs and `RemoveFromRoot`. Do NOT call `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true)` from inside a delegate handler — parallel GC workers race the in-flight delegate's write access and trip `FMRSWRecursiveAccessDetector`. `Shutdown()` is the safe alternative because it only resets the worker `TUniquePtr`; it never touches delegate state.

All dynamic delegate handlers on observer UCLASSes MUST take `FString` **by value**, not `const FString&`. UE's `BindDynamic` does strict method-pointer matching against the delegate's declared signature, and every delegate in this plugin declares `FString` by value. A handler with `const FString&` compiles fine on its own but fails at the `BindDynamic` call site with a cryptic `cannot convert argument` error — learned the hard way during D.1.

### Shared helpers + adding new tests

Shared helpers (model path resolution, JSON parsing, tool-call extraction, assistant text extraction) live in `InoAgentsSmokeTestCommon.{h,cpp}` under the `InoAgentsSmokeTest` namespace. Test-specific helpers live in the test file's anonymous namespace.

To add a new smoke test, drop a new `.cpp` (and optional `.h` for observer UCLASSes) into `Private/SmokeTests/`. UBT auto-picks up `.cpp` files under `Private/`; no `Build.cs` changes needed. `Private/SmokeTests/` is already on the include path via `PrivateIncludePaths`.

Smoke tests are compiled into every build configuration. For now they're gated behind console commands and never run unless explicitly invoked. If any individual test grows shipping-sensitive logic, wrap that file in `#if !UE_BUILD_SHIPPING` as a follow-up change.

## Platform support

Currently **Windows (Win64, MSVC)** only. CPU inference works end-to-end. GPU path (`//c:engine`) builds but is untested — swap the Bazel target when enabling.

Future platforms (Android, iOS, Linux, macOS) each require porting `InoAgentsLibrary.Build.cs` with a platform branch. Upstream `.bazelrc` already has `--config=android_arm64`, `--config=ios_arm64`, and `build:macos_arm64`. The UE API and Bazel recipe are identical across platforms — only the Build.cs branch differs.

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

Gemma 4 `.litertlm` model files are 2.5–5 GB and **must never be committed**. Models are auto-downloaded on first use from URLs configured in Project Settings → Plugins → InoAgents → LiteRT-LM → Models.

### Model path resolution

`LiteRtLmResolveModelPath(ModelFileName)` (in `LiteRtLmTypes.h/.cpp`) checks two locations in order:

1. **`FPaths::ProjectPersistentDownloadDir() / "InoAgents/Models/"`** — where auto-downloaded models are cached. This is UE's canonical location for runtime-acquired content that persists across sessions and app updates. Platform-appropriate (sandboxed on mobile, app-support on macOS).
2. **`Plugins/InoAgents/Models/`** — legacy dev-time path. The plugin's `.gitignore` excludes `Models/` so the 2.5+ GB file never lands in git.

If neither location has the file, `ULiteRtLmSubsystem::LoadModelAsync` looks up the `ModelFileName` in the `UInoAgentsSettings::Models` array to find the download URL, then downloads via `FHttpModule` and saves to `PersistentDownloadDir`. The subsystem fires `OnDownloadProgress(Percent, BytesReceived, TotalBytes)` during download for loading screens.

### Model config

Models are configured via `FLiteRtLmModelConfig` — a **plain USTRUCT** (not a UDataAsset). Set `ModelFileName`, `Backend`, `MaxNumTokens`, `SystemMessage` directly on the agent component's details panel, or build one in Blueprint via a Make node and pass to `LoadModelAsync`.

Phase 1 smoke tests under `InoAgents.*` still hardcode the model path via `InoAgentsSmokeTest::ResolveDefaultModelPath()` because they bypass the UE API and call the C functions directly.

### System message format

The system message is passed to LiteRT-LM's C API as a **plain text string** (NOT wrapped in JSON). The C API's `engine.cc:214-226` tries to JSON-parse the input; when parsing fails (because raw text isn't valid JSON), it falls back to using the raw string as the "content" field: `{"role":"system","content":"Your prompt here..."}`. This is what Gemma's Jinja2 chat template expects — wrapping as `{"type":"text","text":"..."}` would produce a content object that the template silently drops.

### Shipping builds

Auto-download to `PersistentDownloadDir` is **implemented** and works for both dev and shipping:
- First run downloads from the configured Hugging Face URL (~2.5–5 GB, 2–10 min)
- `OnDownloadProgress` fires for loading-screen UI
- Subsequent runs use the cached file (loads in <1 second with XNNPACK cache)
- SHA-256 verification is NOT yet implemented (planned)

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
