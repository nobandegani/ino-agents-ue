# CLAUDE.md — InoAgents plugin

This file provides guidance to Claude Code (claude.ai/code) when working inside `Plugins/InoAgents/`. The hosting demo project is documented in `E:/Projects/InoProject/CLAUDE.md`.

## Purpose

`InoAgents` is an Unreal Engine 5.7 runtime plugin that delivers on-device + cloud AI capabilities to UE games and tools through Blueprint-friendly subsystems:

- **On-device LLM chat with tool calling** via the `InoLiteRtLm` backend module, running Google's LiteRT-LM Gemma 4 engine.
- **Cloud TTS** via ElevenLabs (`/v1/text-to-dialogue/stream`).
- **Supporting helpers** — procedural blink / gaze animation utilities, mono PCM audio helpers (silence / dithered silence / WAV writer), a gyro-driven camera sway component.

The plugin is named for "agents" deliberately: the goal is not just text generation but **tool-use / function-calling workflows** running natively in UE, driven from Blueprint.

## Repository layout

```
Plugins/InoAgents/
├── InoAgents.uplugin                              ← declares "InoLiteRT" + "InoSpeakNG"
│                                                    + "InoNodes" + "RuntimeAudioImporter"
│                                                    + "JsonBlueprintUtilities" in its
│                                                    Plugins array, and the InoAgents +
│                                                    InoLiteRtLm runtime modules
│
├── Source/
│   ├── InoAgents/                                 ← core runtime module
│   │   ├── InoAgents.Build.cs
│   │   ├── Public/
│   │   │   ├── InoAgents.h                        ← module interface (FInoAgentsModule)
│   │   │   ├── InoAgentsLog.h                     ← shared LogInoAgents category
│   │   │   ├── InoAgentsSettings.h                ← UInoAgentsSettings (UDeveloperSettings —
│   │   │   │                                        ElevenLabs only; sub-modules own theirs)
│   │   │   ├── ElevenLabs/                        ← cloud TTS Blueprint surface
│   │   │   │   ├── InoElevenLabsTypes.h           ← request struct, output-format enum, delegates
│   │   │   │   ├── InoElevenLabsSubsystem.h       ← settings cache + live-action registry
│   │   │   │   └── InoElevenLabsTextToDialogueStream.h ← async-action wrapper
│   │   │   ├── Animation/
│   │   │   │   └── InoAnimationBlueprintHelper.h  ← eye-look + procedural blink helpers
│   │   │   ├── Audio/
│   │   │   │   └── InoAudioFunctionLibrary.h      ← silence / dithered-silence / SaveWav helpers
│   │   │   ├── Camera/
│   │   │   │   └── InoGyroCameraSwayComponent.h   ← gyro / tilt-driven camera sway
│   │   │   └── SmokeTests/
│   │   │       └── InoSmokeTestCommon.h           ← shared helpers (model paths, JSON, …) —
│   │   │                                            re-exported via PublicIncludePaths so
│   │   │                                            sibling modules can include it bare
│   │   └── Private/
│   │       ├── InoAgents.cpp                      ← thin module lifecycle (no DLL loading —
│   │       │                                        InoLiteRT pre-loads its runtimes at
│   │       │                                        PreLoadingScreen)
│   │       ├── InoAgentsSettings.cpp
│   │       ├── ElevenLabs/                        ← cloud TTS impl
│   │       │   ├── InoElevenLabsSubsystem.cpp
│   │       │   └── InoElevenLabsTextToDialogueStream.cpp
│   │       ├── Animation/InoAnimationBlueprintHelper.cpp
│   │       ├── Audio/InoAudioFunctionLibrary.cpp
│   │       ├── Camera/InoGyroCameraSwayComponent.cpp
│   │       └── SmokeTests/
│   │           ├── InoSmokeTestCommon.cpp
│   │           ├── InoElevenLabsDialogueStreamTest.h
│   │           └── InoElevenLabsDialogueStreamTest.cpp ← Ino.ElevenLabsDialogueStreamTest
│   │
│   └── InoLiteRtLm/                               ← LiteRT-LM Gemma 4 backend module
│       ├── InoLiteRtLm.Build.cs                   ← deps: InoAgents (Log category +
│       │                                            smoke-test helper), InoLiteRT
│       │                                            (LiteRT + LiteRT-LM C APIs),
│       │                                            InoNodes (downloader / SHA-256),
│       │                                            Json + JsonUtilities +
│       │                                            JsonBlueprintUtilities,
│       │                                            DeveloperSettings
│       ├── Public/
│       │   ├── InoLiteRtLm.h                      ← FInoLiteRtLmModule
│       │   ├── InoLiteRtLmSettings.h              ← UInoLiteRtLmSettings (its own
│       │   │                                        DeveloperSettings page, lives with
│       │   │                                        the module so deletion is one-step)
│       │   └── LiteRtLm/                          ← Blueprint-facing types
│       │       ├── InoLiteRtLmTypes.h             ← enums (Backend, SamplerType,
│       │       │                                    Activation, MessageRole,
│       │       │                                    SentenceSplit, TagStrip),
│       │       │                                    FInoLiteRtLmModelConfig,
│       │       │                                    FInoLiteRtLmModelEntry, all delegates
│       │       ├── InoLiteRtLmSubsystem.h         ← UInoLiteRtLmSubsystem (engine owner,
│       │       │                                    tool registry, model loader)
│       │       ├── InoLiteRtLmConversation.h      ← UInoLiteRtLmConversation (one stateful
│       │       │                                    chat with history, streaming,
│       │       │                                    tool-call agent loop)
│       │       ├── InoLiteRtLmToolBase.h          ← UInoLiteRtLmToolBase (Blueprintable
│       │       │                                    tool base class)
│       │       └── InoLiteRtLmAddNumbersTool.h    ← canonical tool sample
│       └── Private/
│           ├── InoLiteRtLm.cpp                    ← thin module lifecycle
│           ├── InoLiteRtLmSettings.cpp
│           ├── LiteRtLm/
│           │   ├── InoLiteRtLmSubsystem.cpp       ← Blueprint glue, model loader,
│           │   │                                    InoNodes download wiring, tool registry
│           │   ├── InoLiteRtLmConversation.cpp    ← per-conversation state + delegates
│           │   ├── InoLiteRtLmConversationWorker.{h,cpp} ← FRunnable-style worker for the
│           │   │                                    streaming-token agent loop
│           │   ├── InoLiteRtLmTypes.cpp           ← backend-string helper, path resolver
│           │   ├── InoLiteRtLmToolBase.cpp
│           │   ├── InoLiteRtLmAddNumbersTool.cpp
│           │   └── InoLiteRtLmStubs_NonWindows.cpp ← stub C API symbols for platforms
│           │                                       where the LiteRT-LM build hasn't
│           │                                       been staged yet (link-time fallback)
│           └── SmokeTests/
│               ├── InoLoadEngineTest.cpp                  ← Ino.LiteRtLm.LoadEngineTest
│               ├── InoGenerateTest.cpp                    ← Ino.LiteRtLm.GenerateTest
│               ├── InoStreamTest.cpp                      ← Ino.LiteRtLm.StreamTest
│               ├── InoConversationTest.cpp                ← Ino.LiteRtLm.ConversationTest
│               ├── InoToolCallTest.cpp                    ← Ino.LiteRtLm.ToolCallTest
│               ├── InoLiteRtLmSubsystemLoadTest.{h,cpp}   ← Ino.LiteRtLm.SubsystemLoadTest
│               ├── InoLiteRtLmConversationSendTest.{h,cpp}
│               ├── InoLiteRtLmConversationStreamTest.{h,cpp}
│               ├── InoLiteRtLmConversationContextTest.{h,cpp}
│               ├── InoLiteRtLmConversationToolTest.{h,cpp}
│               └── InoLiteRtLmToolRegistryTest.cpp
│
├── DepricatedModules/                             ← retired backends, kept on disk for
│   ├── Chatterbox/                                 reference. Not built. Not in
│   ├── InoChatterboxNative/                        Modules[] of the .uplugin. Cherry-pick
│   ├── InoNeuTtsNative/                            from here if a backend ever comes back
│   ├── InoNeuTtsNativeEditor/                      rather than re-deriving the design.
│   ├── InoQwen3ASRLiteRT/
│   ├── NeuTTS/
│   └── Qwen3ASR/
│
├── Content/                                       ← (UE plugin content — Blueprints, etc.)
├── Resources/                                     ← plugin icon
├── docs/                                          ← any plugin-level docs
└── README.md
```

The runtime-binary side — staging LiteRT + LiteRT-LM, the PreLoadingScreen-phase StartupModule that loads them, per-platform packaging — is owned entirely by the sibling **`InoLiteRT`** plugin. From InoAgents' perspective the LiteRT-LM C API is simply available: `#include "litert/lm/engine.h"`, call into it directly. No DLL-loading work in InoAgents.

## Module split + "self-contained deletion unit" pattern

The plugin uses a **two-tier module split**:

- **`InoAgents` (core)** — the thin always-present surface: shared log category, ElevenLabs cloud TTS, animation / audio / camera helpers, the smoke-test common helper. Does NOT depend on any backend module. Deleting a backend never affects InoAgents core.
- **`InoLiteRtLm` (backend)** — a self-contained deprecation unit. Owns its own `UDeveloperSettings` page (`UInoLiteRtLmSettings`), its own `Private/SmokeTests/`, and consumes the sibling `InoLiteRT` plugin's LiteRT-LM C API directly. To remove the entire LiteRT-LM implementation: delete `Source/InoLiteRtLm/` and drop its entry from `InoAgents.uplugin`'s `Modules` array. Settings page, smoke tests, and subsystem all go with it.

The same pattern is what allowed the previous Chatterbox / NeuTTS / Qwen3 ASR backends to be moved cleanly to `DepricatedModules/` — each was its own module under `Source/`, with its own settings + smoke tests, and no reverse-dependency from `InoAgents` core. The `InoAgents.Build.cs` deliberately does NOT depend on backend modules; game code reaches a backend by fetching its subsystem on demand.

Adding a reverse dep from `InoAgents` to a backend would defeat this property — don't do it. Cross-cutting helpers that need to be shared between core and backends (the `LogInoAgents` category, `InoSmokeTestCommon.h`, `UInoAudioFunctionLibrary`) live in `InoAgents` core and backends depend on `InoAgents`, never the other way round.

## Cross-module include conventions

A handful of conventions in `InoAgents.Build.cs` make sibling-module + private-subdir includes terse without losing greppability:

- **`Public/SmokeTests/` is exposed as a `PublicIncludePaths` entry.** Bare `#include "InoSmokeTestCommon.h"` works from any module that depends on InoAgents.
- **`Public/Audio/InoAudioFunctionLibrary.h` is NOT re-exported.** Cross-module callers use the explicit `#include "Audio/InoAudioFunctionLibrary.h"` so the cross-module nature stays greppable.
- **`Private/SmokeTests/`, `Private/ElevenLabs/`, `Private/Audio/` are on `PrivateIncludePaths`.** Sibling `.cpp` files in InoAgents core can include each other's private headers without relative paths.
- **`InoLiteRtLm`'s `Private/LiteRtLm/` + `Private/SmokeTests/` are on its own `PrivateIncludePaths`.** Same convention scoped to the backend module.

## LiteRT-LM (Gemma 4 chat + tool calling)

`UInoLiteRtLmSubsystem` is the on-device LLM frontend. Owns one `LiteRtLmEngine*` per game instance (the engine is expensive to construct and shared across conversations). Hands out `UInoLiteRtLmConversation` instances; enforces a **single-conversation-per-engine invariant** because LiteRT-LM sessions on the same engine share a single LlmExecutor + KV cache (see `runtime/core/engine_impl.cc:157` in the vendor tree). Creating a new conversation auto-shuts-down any prior live one.

### Lifecycle

```
1. (configure once)  Project Settings → Plugins → InoLiteRtLm → Models[]
                       Each entry: DisplayName, DownloadUrl, LocalFileName,
                       ExpectedSha256, FileSizeBytes, Language, Quantization

2. (async)           Subsys->LoadModelAsync(Config, OnDownloadProgress, OnLoaded)
                       - Resolves a registry entry (DisplayName OR LocalFileName,
                         case-insensitive).
                       - Calls InoNodes::Download::DownloadFileAsync which handles
                         cache check / HEAD probe / .partial staging / atomic
                         rename / streaming SHA-256 / multi-connection range /
                         exponential-backoff retries / cancel tokens.
                       - On download success, ThreadPool-dispatches
                         litert_lm_engine_settings_create + litert_lm_engine_create.
                       - Marshals back to game thread; OnLoaded fires exactly once.

3. (sync)            UInoLiteRtLmToolBase-derived UObjects → Subsys->RegisterTool(...)
                       Each tool is a Blueprintable UCLASS with ToolName, Description,
                       Parameters, and an Execute() override. Registered before the
                       first CreateConversation call.

4. (sync)            UInoLiteRtLmConversation* Conv = Subsys->CreateConversation();
                       OR Subsys->CreateConversationWithHistory(InitialMessages);
                       - Snapshots the currently-registered tools into the new
                         conversation's tools_json. Tools registered later don't
                         apply retroactively.

5. (async, N times)  Conv->SendMessageAsync(UserText, …)
                       - OnUserMessage fires immediately on the game thread.
                       - OnToken fires per streaming chunk (RawText + CleanText —
                         CleanText has [bracketed] tags stripped per
                         EInoLiteRtLmTagStrip).
                       - OnSentence fires at each configured boundary
                         (EInoLiteRtLmSentenceSplit — newline / period / comma /
                         ? / ! / ; / :), with the same RawText / CleanText pair.
                       - OnSentenceBoundary fires after each OnSentence as a
                         payload-less cue for animation / viseme code.
                       - If the model emits a tool call, the conversation looks the
                         tool up in the subsystem registry, calls Execute() on the
                         game thread, feeds the result back into LiteRT-LM, and
                         continues streaming. OnToolCalled fires after each
                         round-trip as a diagnostic.
                       - Terminal: OnComplete (FullText) OR OnError (ErrorMessage),
                         never both.

6. (anytime)         Conv->Cancel()                 cooperative abort
                     Subsys->UnloadModel()          tears down active conversation
                                                     then engine

7. (auto on          Subsys->Deinitialize           cancels in-flight download,
   GameInstance       UnloadModel.
   teardown)
```

### Tool calling

`UInoLiteRtLmToolBase` is Blueprintable. Subclass it, fill in `ToolName` (used as the lookup key), `Description`, `Parameters` (JSON schema string), and implement `Execute(FJsonObjectWrapper Args, FString& OutResultJson)`. Register via `Subsys->RegisterTool(NewObject<MyTool>(...))` before creating the conversation that should see it.

`UInoLiteRtLmAddNumbersTool` is the canonical sample — takes `{ "a": float, "b": float }`, returns `{ "sum": float }`. Use it as the template when adding new tools.

Tool execution runs on the **game thread** — Blueprint Execute() implementations don't need to worry about thread safety against UE state, but they should be fast (a few ms) since they stall token streaming. For longer-running work, dispatch to a worker yourself and stash a continuation; the agent loop is already async on the LiteRT-LM side.

### Sentence + tag handling

Two orthogonal post-processing layers sit between the raw LiteRT-LM token stream and the final delegates:

- **Tag stripping** (`EInoLiteRtLmTagStrip`) — applied per-token. Removes content between configured delimiter pairs from `CleanText`; `RawText` always preserves the original. Default: square brackets + curly braces. Useful for piping `RawText` to ElevenLabs (which consumes `[cheerfully]` as a delivery hint) while showing `CleanText` in subtitles.
- **Sentence splitting** (`EInoLiteRtLmSentenceSplit`) — buffers tokens and fires `OnSentence(RawText, CleanText)` at each configured boundary. Each split flag matches a punctuation character followed by a single space, so numeric literals like `3.14` don't trigger. `OnSentenceBoundary` fires right after each `OnSentence` as a payload-less cue for animation triggers.

Both default sets cover the common case for chat UIs that want to subtitle the model output line-by-line while feeding raw expressive text to TTS.

### Session-config attachment caveat

`FInoLiteRtLmModelConfig::bAttachSessionConfig` defaults **false** because LiteRT-LM `v0.11.0-rc.1` regressed Gemma-4 conversation creation when a user `SessionConfig` was attached (`Conversation::Create` returned NULL). When false, conversations run with engine defaults and `FInoLiteRtLmModelConfig::Sampler` + `MaxOutputTokens` are silently ignored. Flip on to test once the upstream regression is confirmed fixed in `v0.11.0` final — see comment in `InoLiteRtLmConversation.cpp` for the history.

### Model registry

`UInoLiteRtLmSettings::Models` (Project Settings → Plugins → InoLiteRtLm) is a `TArray<FInoLiteRtLmModelEntry>`. Each entry:

| Field | Purpose |
|---|---|
| `DisplayName` | Friendly picker name. Lookup key for `FInoLiteRtLmModelConfig::ModelFileName`, case-insensitive. |
| `DownloadUrl` | Direct GET URL (HuggingFace `…/resolve/main/…`, S3, your CDN). Public URLs only. |
| `LocalFileName` | On-disk name. Must end in `.litertlm`. Fallback lookup key. |
| `ExpectedSha256` | Hex-encoded SHA-256. Optional but strongly recommended — corrupt cache triggers automatic delete + re-download. |
| `FileSizeBytes` | For progress UI when the server omits Content-Length. 0 = HEAD-probe before downloading. |
| `Language` | IETF tag, informational. |
| `Quantization` | Label like `Q4_0` / `Q8_0` / `FP16` — informational only (the real dtype is intrinsic to the file). |

Downloaded files land at `<FPaths::ProjectPersistentDownloadDir()>/InoAgents/LiteRTLM/<LocalFileName>`. `UInoLiteRtLmSubsystem::IsModelDownloaded(NameOrFileName)` is a cheap stat probe (no SHA hash, no I/O beyond `Stat`) — safe to poll from UMG to decide whether to show a download-progress UI before kicking off `LoadModelAsync`.

`LiteRtLmResolveModelPath` (in `InoLiteRtLmTypes.h`) is the existence-checking resolver — checks the persistent-download dir first, then the legacy `Plugins/InoAgents/Models/` dev drop, returns empty string if neither has the file.

## UE-side integration architecture

The UE-facing API lives under `Source/InoAgents/Public/` (cross-cutting helpers + ElevenLabs) and `Source/InoLiteRtLm/Public/` (Gemma 4 chat). Blueprint / C++ callers wire the subsystems together themselves; the demo project's character actor is the integration point.

```
Blueprint / C++ ─┬─ UInoLiteRtLmSubsystem            (UGameInstanceSubsystem)
                 │     Owns the LiteRtLmEngine* + the tool registry +
                 │     the single active conversation invariant.
                 │     LoadModelAsync auto-downloads via InoNodes (HEAD-probe
                 │     + GET + .partial staging + atomic rename + streaming
                 │     SHA-256 + multi-connection range + retries + cancel
                 │     token); fires OnDownloadProgress on the game thread.
                 │     CreateConversation / CreateConversationWithHistory
                 │     spawn a UInoLiteRtLmConversation bound to the engine.
                 │     RegisterTool / UnregisterTool / FindTool — Blueprintable
                 │     tool registry; schema snapshotted into each conversation
                 │     at creation time.
                 │
                 ├─ UInoLiteRtLmConversation         (UObject, caller-owned)
                 │     One stateful chat. SendMessageAsync drives the streaming-
                 │     token agent loop on a worker thread:
                 │       OnUserMessage  — game thread, before the model starts.
                 │       OnToken        — per chunk; RawText + CleanText pair.
                 │       OnSentence     — per configured boundary
                 │                        (EInoLiteRtLmSentenceSplit bitmask).
                 │       OnSentenceBoundary — payload-less cue right after.
                 │       OnToolCalled   — diagnostic, after each tool round-trip.
                 │       OnComplete / OnError — terminal, mutually exclusive.
                 │     Cancel() — cooperative abort.
                 │
                 ├─ UInoLiteRtLmToolBase             (Blueprintable UCLASS)
                 │     Base class for tools. Subclass, set ToolName / Description
                 │     / Parameters, override Execute(JsonArgs, OutResultJson).
                 │     UInoLiteRtLmAddNumbersTool is the canonical sample.
                 │
                 ├─ UInoElevenLabsSubsystem          (UGameInstanceSubsystem)
                 │     Caches settings (API key, base URL, default model id,
                 │     default output format) at PIE start; anchors live HTTP
                 │     actions via a UPROPERTY TSet so they survive GC; fires
                 │     CancelAll() at Deinitialize so post-PIE responses can't
                 │     dispatch into freed UObjects.
                 │
                 ├─ UInoElevenLabsTextToDialogueStream  (UBlueprintAsyncActionBase)
                 │     Latent node for POST /v1/text-to-dialogue/stream.
                 │     OnAudioChunk (delta bytes) / OnComplete (full bytes) /
                 │     OnError. Multiple output formats via
                 │     EInoElevenLabsOutputFormat (MP3 / PCM / u-law).
                 │     Caller owns playback — there is no built-in audio
                 │     component; feed the bytes into RuntimeAudioImporter
                 │     (UStreamingSoundWave::AppendAudioDataFromRAW) or your
                 │     own audio pipeline.
                 │
                 ├─ UInoAgentsSettings               (UDeveloperSettings)
                 │     Project Settings → Plugins → InoAgents — ElevenLabs only:
                 │       ApiKey, BaseUrl, DefaultModelId, DefaultOutputFormat.
                 │     Sub-module-specific settings (UInoLiteRtLmSettings) live
                 │     in their own UDeveloperSettings pages owned by their
                 │     module — deleting a sub-module's directory + its
                 │     .uplugin entry also removes its settings page.
                 │
                 ├─ UInoAnimationBlueprintHelper     (UBlueprintFunctionLibrary)
                 │     Stateless animation helpers exposed for the demo
                 │     character:
                 │       CalculateGaze — merged eye + head tracking. Returns
                 │         ARKit-style eye blend shape weights AND a local-space
                 │         head FRotator, with a three-state attention machine
                 │         (Tracking / EyeGlance / FullGlance) for natural
                 │         periodic look-away behaviour. Two bApply* flags let
                 │         the caller decide whether to use the outputs or fall
                 │         back to the base animation for eyes and head
                 │         independently.
                 │       CalculateBlinkWeight — procedural blink state machine:
                 │         random intervals with occasional long "stare" pauses,
                 │         asymmetric ease-in close / ease-out open, double
                 │         blinks, SpeakingIntensity input to accelerate rate
                 │         during speech.
                 │     Pass the previous frame's output back as PreviousState /
                 │     PreviousResult for frame-rate-independent smoothing.
                 │
                 ├─ UInoAudioFunctionLibrary         (UBlueprintFunctionLibrary)
                 │     GenerateEmptyRawAudio (silent PCM in any
                 │     ERuntimeRAWAudioFormat — note that unsigned PCM uses the
                 │     midpoint as silence, not zero); GenerateDitheredSilence
                 │     (low-amplitude white noise so neural lip-sync models stay
                 │     in their training distribution during pauses, default
                 │     ~-76 dBFS); SaveInt16PcmAsWav (write PCM bytes + RIFF
                 │     header — useful for capturing TTS output to disk for
                 │     verification).
                 │
                 └─ UInoGyroCameraSwayComponent      (USceneComponent)
                     BlueprintSpawnableComponent for parenting under a camera
                     rig. Reads APlayerController::GetInputMotionState's Tilt
                     vector (more stable than raw gyro RotationRate) and applies
                     a per-axis rotation offset to its own transform; children
                     (a CameraComponent, an overlay) inherit the sway. On
                     desktop without a motion sensor, Tilt is zero and the
                     component is a clean no-op — safe to leave in for cross-
                     platform builds without #if PLATFORM_* guards. First non-
                     zero sample latches as "rest" so the camera starts centred
                     regardless of phone-orientation-at-launch.
```

## Smoke tests

Development-time console commands that exercise the UE-facing API surface end-to-end through PIE. They register themselves as `FAutoConsoleCommand` globals at file scope so they become available the moment the module's DLL loads.

Invoke from the editor's Output Log command input.

### LiteRT-LM (on-device chat + tool calling)

Under `Source/InoLiteRtLm/Private/SmokeTests/`. The granular tests stage on intermediate steps (engine load → one-shot generate → streaming → multi-turn conversation → tool calls) so a regression at any layer is bisectable without running the full agent loop.

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.LiteRtLm.LoadEngineTest [model name]` | Resolves Project Settings → loads the `.litertlm` engine. Logs engine ptr + load time. | no |
| `Ino.LiteRtLm.GenerateTest [model] [prompt...]` | One-shot generate via the raw C API (bypasses the subsystem). Logs full reply + tokens/sec. | no |
| `Ino.LiteRtLm.StreamTest [model] [prompt...]` | Streaming variant of the above — per-token arrival timing + TTFT. | no |
| `Ino.LiteRtLm.ConversationTest [model] [turns...]` | Multi-turn conversation via the C API directly. Verifies KV cache reuse across turns. | no |
| `Ino.LiteRtLm.ToolCallTest [model]` | Registers a stub tool, sends a prompt that elicits the call, verifies the round-trip. | no |
| `Ino.LiteRtLm.SubsystemLoadTest [model name]` | End-to-end via `UInoLiteRtLmSubsystem::LoadModelAsync` (download, SHA-verify, engine_create, OnLoaded). | **yes** |
| `Ino.LiteRtLm.ConversationSendTest [model] [prompt...]` | End-to-end via `Subsys->CreateConversation` + `Conv->SendMessageAsync`. Logs token / sentence / complete callbacks. | **yes** |
| `Ino.LiteRtLm.ConversationStreamTest [model] [prompt...]` | Same path with explicit per-token / per-sentence timing log. | **yes** |
| `Ino.LiteRtLm.ConversationContextTest [model]` | Exercises `CreateConversationWithHistory` with seeded turns. | **yes** |
| `Ino.LiteRtLm.ConversationToolTest [model]` | Subsystem-level tool round-trip: RegisterTool → CreateConversation → SendMessageAsync → OnToolCalled. | **yes** |
| `Ino.LiteRtLm.ToolRegistryTest` | Register + lookup + unregister + double-register-overwrites semantics on the subsystem's tool registry. | no |

### ElevenLabs (cloud TTS)

| Command | What it proves | PIE? |
|---|---|---|
| `Ino.ElevenLabsDialogueStreamTest` | Streams a short two-line dialogue through `UInoElevenLabsTextToDialogueStream`, logs total bytes received + chunk count + per-format header bytes. Requires `ElevenLabsApiKey` set in Project Settings. | **yes** |

### Shared helpers + adding new tests

Shared helpers (model path resolution, JSON parsing, tool-call extraction, assistant text extraction) live in `InoSmokeTestCommon.{h,cpp}` under the `InoSmokeTest` namespace, re-exported via the InoAgents core's `PublicIncludePaths` so backend modules can `#include "InoSmokeTestCommon.h"` bare. Test-specific helpers live in the test file's anonymous namespace.

To add a new smoke test, drop a new `.cpp` (and optional `.h` for observer UCLASSes) into the appropriate module's `Private/SmokeTests/`. UBT auto-picks up `.cpp` files; no `Build.cs` changes needed.

Smoke tests are compiled into every build configuration. For now they're gated behind console commands and never run unless explicitly invoked. If any individual test grows shipping-sensitive logic, wrap that file in `#if !UE_BUILD_SHIPPING` as a follow-up change.

`InoLiteRtLm.Build.cs` disables unity build (`bUseUnity = false`) because anonymous-namespace constants in the Phase-1 smoke tests collide when merged into a single unity TU. Build-time cost is small (~20 .cpps); leave the flag in place when adding new tests under that module.

### Observer-UCLASS conventions

Every UE API smoke test that observes async delegates uses the same pattern: `NewObject` + `AddToRoot`, bind dynamic delegates via `AddDynamic`, run the workflow, and in `Finish()` call the runner's `Shutdown()` for deterministic teardown before clearing UPROPERTY refs and `RemoveFromRoot`. Do NOT call `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true)` from inside a delegate handler — parallel GC workers race the in-flight delegate's write access and trip `FMRSWRecursiveAccessDetector`. `Shutdown()` is the safe alternative because it only resets the worker `TUniquePtr`; it never touches delegate state.

All dynamic delegate handlers on observer UCLASSes MUST take `FString` **by value**, not `const FString&`. UE's `BindDynamic` does strict method-pointer matching against the delegate's declared signature, and every delegate in this plugin declares `FString` by value. A handler with `const FString&` compiles fine on its own but fails at the `BindDynamic` call site with a cryptic `cannot convert argument` error.

## Platform support

Sibling-plugin owned. `Plugins/InoLiteRT/CLAUDE.md` lists the per-platform support matrix and which backends (CPU / GPU / NPU) ship in each artifact. From InoAgents' perspective the LiteRT-LM C API is either available on the platform or not, and InoAgents code calls into it via `#include "litert/lm/engine.h"` in either case.

The UE API (subsystems, delegates) is **identical across platforms**. On platforms where InoLiteRT hasn't staged a build, `InoLiteRtLmStubs_NonWindows.cpp` provides link-time fallbacks that return null/error so the module still loads cleanly and `LoadModelAsync` errors out with a clear `OnLoaded(false, "...")`. Every non-LiteRT-LM feature (ElevenLabs cloud TTS, animation/audio/camera helpers) keeps working normally on every platform.

### Android specifics

All native packaging on Android — UPL XML, `<soLoadLibrary>` order, GPU/NPU accelerator staging — is owned by the sibling `InoLiteRT` plugin at `LoadingPhase=PreLoadingScreen`. By the time `FInoAgentsModule::StartupModule` runs (Default phase), the LiteRT runtime is mapped into the process. What InoAgents itself owns on Android:

- **Model file distribution.** Multi-GB `.litertlm` model files cannot ship inside the APK (Play Store limit is 200 MB base APK). `UInoLiteRtLmSubsystem::LoadModelAsync` auto-downloads to `FPaths::ProjectPersistentDownloadDir()` on first use; `android.permission.INTERNET` is required (already enabled for ElevenLabs).

## How to update the runtime versions

Runtime version bumps happen in the sibling `InoLiteRT` plugin, not here — see `Plugins/InoLiteRT/CLAUDE.md` for that plugin's update script and watch-outs. After a LiteRT-LM bump, re-run InoAgents' UE-API smoke tests (`Ino.LiteRtLm.*`) to confirm the subsystem still talks to the new C API correctly. The `bAttachSessionConfig` regression note in the "Session-config attachment caveat" section above is the kind of thing that should be re-verified after each bump.

## What to verify before trusting this file

This file describes design decisions and architectural intent. Specifics drift over time. Before acting on any specific claim:

- **LiteRT-LM version + C API:** owned by `InoLiteRT` — check `Plugins/InoLiteRT/CLAUDE.md` for the pinned version, the staged `litert/lm/engine.h`, and the runtime DLLs/.so it provides.
- **Session-config regression:** the `bAttachSessionConfig = false` default in `FInoLiteRtLmModelConfig` exists because of a specific `v0.11.0-rc.1` issue. Check `InoLiteRtLmConversation.cpp` for the comment thread, and re-verify after every LiteRT-LM bump.
- **Deprecated backends:** anything described here as "removed" or "moved to DepricatedModules/" reflects the state at the time of writing. If a future task touches that area, check `git log Plugins/InoAgents/DepricatedModules/` and the `.uplugin` `Modules[]` to confirm whether the backend has been resurrected.
