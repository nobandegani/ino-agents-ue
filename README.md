# InoAgents

Unreal Engine 5.7 runtime plugin with three independent integrations:

1. **LiteRT-LM / Google Gemma 4** — on-device tool-calling LLM agents running directly inside the game process. No network. No cloud. No subscription. No Python runtime. No second binary to ship.
2. **ElevenLabs cloud voice API** — standalone HTTP client for ElevenLabs' audio endpoints, exposed as native Blueprint latent nodes and C++ async actions. Phase 1 ships Text-to-Dialogue streaming; TTS and STT are on the roadmap.
3. **Streaming audio playback component** — a `UAudioComponent` subclass that plays raw audio bytes (PCM int16, PCM float32, or MP3) fed in at runtime. Inherits every standard UAudioComponent feature (volume, pitch, attenuation, spatialization, source effect chain, sound class, concurrency). Decoupled from the other two — games that only need MP3 playback can use it without touching the AI or cloud paths.

All three integrations are fully decoupled — use any combination, or none. The plugin is designed Blueprint-first: every surface a gameplay programmer or designer needs is callable or bindable from Blueprint without writing C++.

**Status:** Milestone D (LiteRT-LM UE API) is complete. ElevenLabs phase 1 (Text-to-Dialogue stream) is complete. The streaming audio component is complete. See `CLAUDE.md` for architecture details and `docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md` for the LiteRT-LM design record.

---

## Table of contents

- [What you can do with it](#what-you-can-do-with-it)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [LiteRT-LM API surface](#litert-lm-api-surface)
  - [`ULiteRtLmSubsystem`](#ulitertlmsubsystem--game-instance-subsystem)
  - [`ULiteRtLmConversation`](#ulitertlmconversation)
  - [`ULiteRtLmModelConfig`](#ulitertlmmodelconfig--designer-asset)
  - [`ILiteRtLmTool`](#ilitertlmtool--blueprint-interface)
  - [`ULiteRtLmAddNumbersTool`](#ulitertlmaddnumberstool--reference-tool)
- [Writing your own tool](#writing-your-own-tool)
  - [In Blueprint](#in-blueprint)
  - [In C++](#in-c)
- [Smoke tests](#smoke-tests)
- [ElevenLabs integration (phase 1)](#elevenlabs-integration-phase-1)
  - [Configure the API key](#configure-the-api-key)
  - [`UElevenLabsSettings`](#uelevenlabssettings--project-settings)
  - [`UElevenLabsSubsystem`](#uelevenlabssubsystem--game-instance-subsystem)
  - [`UElevenLabsTextToDialogueStream`](#uelevenlabstexttodialoguestream--latent-async-action)
  - [Blueprint usage](#blueprint-usage-elevenlabs)
  - [C++ usage](#c-usage-elevenlabs)
  - [Smoke test](#elevenlabs-smoke-test)
- [Streaming audio playback](#streaming-audio-playback)
  - [`UInoAgentsStreamingAudioComponent`](#uinoagentsstreamingaudiocomponent--uaudiocomponent-subclass)
  - [Format, sample rate, and channels — the three axes](#format-sample-rate-and-channels--the-three-axes)
  - [Blueprint usage](#blueprint-usage-audio)
  - [C++ usage](#c-usage-audio)
  - [ElevenLabs → audio component wiring](#elevenlabs--audio-component-wiring)
  - [Smoke tests](#streaming-audio-smoke-tests)
- [Known limitations](#known-limitations)
- [Further reading](#further-reading)

---

## What you can do with it

**On-device LLM (LiteRT-LM / Gemma 4):**

- **Load a Gemma 4 `.litertlm` model asynchronously** without freezing the editor or the game thread.
- **Stream assistant responses token-by-token** into UMG widgets, with `OnToken`, `OnComplete`, and `OnError` multicast delegates.
- **Cancel an in-flight reply** mid-stream.
- **Define tools in Blueprint or C++** that implement the `ILiteRtLmTool` interface with a name, a JSON schema, and an `Execute(ArgumentsJson)` method.
- **Register tools globally on the subsystem.** Every conversation created after registration automatically advertises them to the model via constrained decoding.
- **Let the model call your tools.** When the model emits a tool call, the plugin executes your tool's `Execute` method on the game thread (so you can freely touch actors, components, and world state), feeds the result back into the conversation, and streams the final answer the model produces using your tool's result.
- **Observe the full agent loop** via an `OnToolCalled` diagnostic delegate if you want a debug UI or validation assertions.

**Cloud voice (ElevenLabs):**

- **Synthesise multi-speaker dialogue** via a single Blueprint latent node. Pass an array of `{voice_id, text}` pairs; receive audio bytes back as they stream in from ElevenLabs.
- **Stream audio chunks in real time** via an `OnAudioChunk` delegate that fires as each HTTP progress tick lands — useful for low-latency playback, progress bars, or piping into your own audio buffer.
- **Cancel in-flight requests** individually or all-at-once via a single subsystem call. PIE-end teardown is automatic.
- **Store the API key in Project Settings** with a `PasswordField`-masked developer setting. Per-call override parameter lets you fetch keys from your own secret store at runtime.

**Streaming audio playback:**

- **Play raw audio bytes at runtime** through a `UAudioComponent` subclass — drop it onto any actor via Add Component, hand it a byte buffer, hear sound.
- **Multiple formats:** PCM 16-bit signed (the universal default), PCM 32-bit float (for DSP pipeline outputs), and MP3 (decoded on the fly via a bundled minimp3 single-header library).
- **True chunked streaming** — `FeedAudioBytes` is callable many times as chunks arrive; playback starts as soon as the first frame decodes, not after the whole buffer is assembled.
- **Every UAudioComponent feature inherited for free:** volume multiplier, pitch multiplier, attenuation settings, 3D spatialization, source effect chain, sound class, concurrency, Play/Stop/Pause/FadeIn/FadeOut.
- **Zero coupling to ElevenLabs or LiteRT-LM.** A game that only uses this component to play downloaded MP3s from an HTTP server — no AI involved — is a fully supported use case.

All of this is reachable from Blueprint. The only native code you ever need to write is the `Execute` body of a C++ tool — and you can skip even that by implementing tools entirely in Blueprint.

---

## Requirements

| Thing | Why |
|---|---|
| **Unreal Engine 5.7** | Minimum tested version. Earlier UE versions have incompatible subsystem / delegate APIs. |
| **Windows (Win64, MSVC)** | The only platform currently supported. Android, iOS, Linux, and macOS are on the roadmap — the Bazel recipe already has configs for all four, only the `InoAgentsLibrary.Build.cs` branches need porting. |
| **DirectX 12** | LiteRT-LM's GPU path goes through D3D12 on Windows. The CPU path (the default in the current build) has no graphics requirement. |
| **A built `LiteRtLm.dll`** | The C API runtime. Built once from source via `LiteRtLm/scripts/build-win64.ps1`. ~17 MB. See `CLAUDE.md → Build system`. |
| **A Gemma 4 model file** | Not redistributed. Download from Hugging Face — see [Quick start](#quick-start) below. |

Model choices (Apache 2.0, public, no gating):

| Variant | Effective params | Size | Modalities |
|---|---|---|---|
| `gemma-4-E2B-it.litertlm` | ~2 B | **~2.6 GB** | Text + Image |
| `gemma-4-E4B-it.litertlm` | ~4 B | ~5 GB | Text + Image + Audio |

Gemma 4 31B dense and 26B A4B MoE are **not** supported — they are server-class models, not edge models, and LiteRT-LM is an edge runtime.

---

## Quick start

### 1. Get the plugin into your project

Drop `Plugins/InoAgents/` into your project's `Plugins/` directory (or copy this whole `InoAgentDemo` project and use it as a starting point — it is the plugin's demo host).

### 2. Build `LiteRtLm.dll` (one-time)

```powershell
cd Plugins/InoAgents/LiteRtLm
./scripts/setup.ps1     # preflight: checks Bazel, MSVC, BAZEL_VC, etc.
./scripts/build-win64.ps1
```

This runs Bazel, produces `LiteRtLm.dll` + `libGemmaModelConstraintProvider.dll`, and copies both into `Plugins/InoAgents/Binaries/ThirdParty/InoAgentsLibrary/Win64/`. Expect 15–40 minutes on a cold build.

### 3. Download a Gemma 4 model

```powershell
mkdir Plugins/InoAgents/Models
# Download gemma-4-E2B-it.litertlm from:
#   https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm
# Put the file at Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm
```

The `Models/` directory is `.gitignore`d — the file never lands in source control.

### 4. Enable the plugin

Open `YourProject.uproject`, add `InoAgents` to the `Plugins` array with `"Enabled": true`, and regenerate Visual Studio project files.

### 5. Verify it works (smoke test)

1. Launch the editor.
2. Open the **Output Log** (`Window → Output Log`).
3. Look at the log spam on startup:
   ```
   LogInoAgents: InoAgents: loaded libGemmaModelConstraintProvider.dll from ...
   LogInoAgents: InoAgents: loaded LiteRtLm.dll from ...
   LogInoAgents: InoAgents: smoke test passed — litert_lm_set_min_log_level(0) returned cleanly.
   ```
   If you see `ERROR: Failed to load ...dll`, your DLLs aren't in `Binaries/ThirdParty/InoAgentsLibrary/Win64/`. Re-run `build-win64.ps1`.
4. Press **Play** (PIE) — `ULiteRtLmSubsystem` only exists while a game instance is live.
5. In the Output Log's command input (the bar at the bottom), type:
   ```
   InoAgents.LiteRtLm.ConversationToolTest
   ```
6. Watch the log. Within ~3–5 seconds you should see:
   ```
   LogInoAgents: add_numbers: 27 + 15 = 42
   LogInoAgents: ConversationToolTest: OnToolCalled #1 ... result=42
   LogInoAgents: ConversationToolTest: token  1 (+0.000 s) "27"
   LogInoAgents: ConversationToolTest: token  2 (+0.045 s) " plus"
   ...
   LogInoAgents: ConversationToolTest: final answer: "27 plus 15 is 42."
   LogInoAgents: ConversationToolTest: PASS — tool round-trip succeeded ...
   ```
   If you see `PASS`, everything is working — the full agent loop just ran end-to-end.

### 6. Use it from Blueprint

Typical Blueprint flow (describing nodes, not screenshots):

1. On **BeginPlay** in some actor: `Get Game Instance → Get Subsystem (ULiteRtLmSubsystem)`.
2. Create a `ULiteRtLmModelConfig` data asset (Content Browser → Add → Miscellaneous → Data Asset → `LiteRtLmModelConfig`). Set `ModelFileName` to `gemma-4-E2B-it.litertlm`, set a `SystemMessage` like "You are a helpful in-game assistant."
3. Call `LoadModelAsync (Config, OnLoaded)`. Bind a custom event to `OnLoaded`.
4. In the loaded handler: `CreateConversation` → store the returned `ULiteRtLmConversation` in a variable. Bind `OnToken`, `OnComplete`, `OnError` on the conversation.
5. From UI, when the player submits a message: `Send Message Async (UserText)`.
6. In your `OnToken` handler: append `Chunk` to a UMG text widget. Use `IsStreamingInFlight` to keep the Send button disabled while the model is replying.
7. In your `OnComplete` handler: the full text is also available as `FullText` (identical to the concatenation of all tokens).

---

## LiteRT-LM API surface

Every class and function listed here is exposed to Blueprint unless explicitly marked `(C++ only)`. Categories are `InoAgents|LiteRT-LM` and `InoAgents|LiteRT-LM|Tools` in the Blueprint context menu.

### `ULiteRtLmSubsystem` — game instance subsystem

Game-instance-wide owner of the LiteRT-LM engine and tool registry. One instance per game instance; access via `GetGameInstance()->GetSubsystem<ULiteRtLmSubsystem>()` in C++ or `Get Game Instance Subsystem (ULiteRtLmSubsystem)` in Blueprint.

| Function | Kind | Purpose |
|---|---|---|
| `LoadModelAsync(Config, OnLoaded)` | `BlueprintCallable` | Start an async engine load from a `ULiteRtLmModelConfig`. Returns immediately; `OnLoaded` fires on the game thread with `bSuccess` + `ErrorMessage`. |
| `IsModelLoaded()` | `BlueprintPure` | True after a successful `LoadModelAsync` completes, until `UnloadModel` is called. |
| `UnloadModel()` | `BlueprintCallable` | Destroy the loaded engine. **Destroy all live conversations first** — they hold native pointers into the engine. |
| `CreateConversation()` | `BlueprintCallable` | Construct a `ULiteRtLmConversation` bound to the currently loaded engine, with a snapshot of the currently registered tools. The caller owns the returned object (hold a UPROPERTY reference to keep it alive). |
| `RegisterTool(Tool)` | `BlueprintCallable` | Add a tool to the global registry. Schema is validated and the `function.name` field must match `GetToolName()`. Registering an already-registered name replaces the previous entry with a warning. |
| `UnregisterTool(ToolName)` | `BlueprintCallable` | Remove a tool by name. No-op if the name isn't registered. Does NOT retroactively affect conversations that already snapshotted the tool at creation time. |
| `FindTool(ToolName)` | `BlueprintPure` | Look up a tool by name. Returns null if not registered. |
| `BuildToolsJsonForConversation()` | `(C++ only)` | Serialises every registered tool's schema into a JSON array. Used internally by `CreateConversation` to build the `tools_json` parameter. You do not need to call this directly. |

### `ULiteRtLmConversation`

One stateful conversation with the model. Constructed by `ULiteRtLmSubsystem::CreateConversation`, never by `NewObject` directly. Holds a pinned worker thread and a native `LiteRtLmConversation*`.

| Function | Kind | Purpose |
|---|---|---|
| `SendMessageAsync(UserText)` | `BlueprintCallable` | Send a user message. Returns immediately. The worker runs the full agent loop (tool calls included) and fires `OnToken` / `OnToolCalled` / `OnComplete` / `OnError` on the game thread. Multiple calls are queued FIFO. |
| `Cancel()` | `BlueprintCallable` | Abort the currently running send. The terminal broadcast becomes `OnError("Cancelled by caller")`. Does NOT drain queued messages — to abort everything, call `Shutdown`. |
| `Shutdown()` | `BlueprintCallable` | Deterministic, synchronous release of the worker thread and native resources. After `Shutdown` the conversation is a zombie — `SendMessageAsync` errors out. Safe to call from inside a delegate handler. Use when you need immediate cleanup without waiting for GC (scene transitions, tests, explicit lifetime control). |
| `IsStreamingInFlight()` | `BlueprintPure` | True while a send is actively generating. Useful for disabling a "Send" button while the model is replying, or showing a typing indicator. |
| `SubmitDeferredToolResult(Id, ResultJson)` | `BlueprintCallable` | **Stubbed.** Reserved for async tools that need to do their own I/O before answering. Logs a warning and no-ops in the current release. Declared now so Blueprint code can wire it up before the implementation lands. |

Multicast delegates (all `BlueprintAssignable`, all fire on the game thread, all ordered so that `OnToken` / `OnToolCalled` broadcasts for a send always precede the terminal `OnComplete` / `OnError` for that same send):

| Delegate | Params | Fires |
|---|---|---|
| `OnToken` | `(FString Chunk)` | Zero or more times per send, as the model streams the final text response. Tokens emitted during intermediate tool-call rounds are suppressed — you only see tokens from the final answer. |
| `OnComplete` | `(FString FullText)` | Exactly once on success. `FullText` is the full accumulated text of the final round, equal to concatenating every `OnToken` `Chunk` for this send. |
| `OnError` | `(FString ErrorMessage)` | Exactly once on failure. Mutually exclusive with `OnComplete`. |
| `OnToolCalled` | `(FName ToolName, FString ArgumentsJson, FString ResultJson)` | Zero or more times per send, once per tool executed. Purely diagnostic — you never need to bind this for tool calling to work. Useful for debug UI and validation. |

### `ULiteRtLmModelConfig` — designer asset

A `UDataAsset` subclass. Create one per model you want to use — right-click in the Content Browser → Miscellaneous → Data Asset → `LiteRtLmModelConfig`.

| Field | Type | Default | Notes |
|---|---|---|---|
| `ModelFileName` | `FString` | `"gemma-4-E2B-it.litertlm"` | Filename resolved relative to `Plugins/InoAgents/Models/`. The subsystem prepends `IPluginManager::FindPlugin("InoAgents")->GetBaseDir() + "/Models/"` at runtime so the config is portable across developer machines. |
| `Backend` | `ELiteRtLmBackend` | `Cpu` | `Cpu` or `Gpu`. Current CPU-only Bazel build ignores `Gpu` — set it in advance of the GPU target being enabled. |
| `MaxNumTokens` | `int32` | `0` | Upper bound on decode-step tokens. `0` means "use engine default". |
| `SystemMessage` | `FString` (MultiLine) | *(empty)* | Plain text. The subsystem wraps it in the `{"type":"text","text":"..."}` JSON shape internally — do not include JSON braces. |

### `ILiteRtLmTool` — Blueprint interface

Three `BlueprintNativeEvent`s. Either C++ classes (via `_Implementation` overrides) or Blueprint classes (via the Blueprint editor's "New event" on implementing classes) can provide the body.

| Method | Signature | Purpose |
|---|---|---|
| `GetToolName` | `() → FName` | Unique identifier the model uses when emitting a call. Short, `snake_case`. Must match the `function.name` field in the schema. |
| `GetToolSchemaJson` | `() → FString` | OpenAI-style function-call schema as a JSON string. See [Writing your own tool](#writing-your-own-tool) for the exact shape. |
| `Execute` | `(FString ArgumentsJson) → FString` | Synchronous tool body. Runs on the game thread — free to touch actors, components, world state. Return a JSON literal (number, string, object, array) that embeds verbatim into the model's `tool_response.value`. On error, return `"\"ERROR: description\""` (a quoted JSON string literal). |

### `ULiteRtLmAddNumbersTool` — reference tool

C++ implementation of `ILiteRtLmTool` that adds two integers. Doubles as the smoke-test fixture for `InoAgents.LiteRtLm.ConversationToolTest` and as a cargo-cult template for plugin consumers authoring their own tools in C++. Read `Public/LiteRtLm/LiteRtLmAddNumbersTool.h` and `Private/LiteRtLm/LiteRtLmAddNumbersTool.cpp` for the full minimal example.

---

## Writing your own tool

### In Blueprint

1. **Create a Blueprint class** that implements the `LiteRtLmTool` interface. In the Class Settings for any Blueprint class, click Add → Implemented Interfaces → `LiteRtLmTool`.
2. **Override the three interface events** in the My Blueprint panel: `GetToolName`, `GetToolSchemaJson`, `Execute`.
3. **Return the tool name** from `GetToolName` — something short like `get_player_location`.
4. **Return the schema** from `GetToolSchemaJson`. Use a Make String node with the following shape (filled in for your tool):
   ```json
   {
     "type": "function",
     "function": {
       "name": "get_player_location",
       "description": "Returns the player pawn's current world-space XYZ. Use this when the user asks where the player is.",
       "parameters": {
         "type": "object",
         "properties": {},
         "required": []
       }
     }
   }
   ```
   For zero-arg tools, the `parameters.properties` object can be empty. For multi-arg tools, add a property per argument with `"type": "integer" / "number" / "string"` and a short description. Keep the descriptions direct and imperative — Gemma 4 E2B is reliably steered by them.
5. **Implement `Execute`.** `ArgumentsJson` is a JSON object as a string. Use the `FJsonObjectNode` Blueprint library (from UE's built-in JSON helpers) or the `ParseIntoArray` / `FindSubstring` string nodes to pull out arguments. Return a JSON literal as a string — `Printf`-style `"{"x":1.0,"y":2.0,"z":3.0}"` for an object, `"42"` for a bare number, `"\"hello\""` for a quoted string.
6. **Register an instance.** From wherever you initialize your game (e.g. a GameInstance subclass's `Init` or an actor's `BeginPlay`):
   - `Construct Object From Class (MyToolBlueprint)` → store the result.
   - `Get Game Instance → Get Subsystem (ULiteRtLmSubsystem) → Register Tool (<your tool instance>)`.
7. **Use the conversation normally.** The next conversation you create via `CreateConversation` will see the tool in its `tools_json`; the model can now call it.

### In C++

1. **Create a `UObject` subclass** that also inherits `ILiteRtLmTool`. Use `ULiteRtLmAddNumbersTool` as a template:
   ```cpp
   // MyTool.h
   UCLASS(BlueprintType)
   class YOURMODULE_API UMyTool : public UObject, public ILiteRtLmTool
   {
       GENERATED_BODY()
   public:
       virtual FName   GetToolName_Implementation() const override;
       virtual FString GetToolSchemaJson_Implementation() const override;
       virtual FString Execute_Implementation(const FString& ArgumentsJson) override;
   };
   ```
2. **Implement the three methods** in the `.cpp`. Use UE's JSON helpers (`FJsonObject`, `TJsonReaderFactory`, `FJsonSerializer`) to parse `ArgumentsJson` and build the result.
3. **Register an instance** the same way Blueprint does:
   ```cpp
   UMyTool* Tool = NewObject<UMyTool>();
   Subsystem->RegisterTool(TScriptInterface<ILiteRtLmTool>(Tool));
   ```
   Store the `UMyTool*` somewhere that keeps it alive (e.g. a `UPROPERTY` on your GameInstance subclass) — the subsystem holds a `TScriptInterface` reference, which is GC-safe as long as something else also holds a strong reference.

**Error handling.** If parsing fails or the tool encounters an error, return a JSON string literal of the form `"\"ERROR: <description>\""`. The conversation worker forwards the error to the model verbatim, and the model is expected to recover (usually by explaining the failure to the user). Do not `throw` from `Execute` — the worker wraps the call in a `try`/`catch` but you should not rely on that.

**Do NOT call `SendMessageAsync` on the same conversation from inside an `Execute` body.** The worker is mid-round waiting for your tool result; queueing a new message from inside `Execute` will not be serviced until the current send completes, which doesn't happen until `Execute` returns. (It doesn't deadlock, but the new message is processed much later than you'd expect.)

---

## Smoke tests

Five PIE-only console commands under the `InoAgents.LiteRtLm.*` namespace validate the whole API surface end-to-end. Run from the Output Log command input **while in PIE**.

| Command | What it checks |
|---|---|
| `InoAgents.LiteRtLm.SubsystemLoadTest` | Async model load + `OnLoaded` delegate |
| `InoAgents.LiteRtLm.ConversationSendTest` | Non-streaming round-trip ("What is 2 plus 2?") via `OnComplete` |
| `InoAgents.LiteRtLm.ConversationStreamTest [prompt]` | Streaming via `OnToken` — default prompt is a haiku |
| `InoAgents.LiteRtLm.ToolRegistryTest` | Tool registry CRUD without loading the model |
| `InoAgents.LiteRtLm.ConversationToolTest [prompt]` | Full agent loop with `add_numbers` — the headline test |

Each test pastes a detailed log to the Output Log. Read the test's `.cpp` file under `Source/InoAgents/Private/SmokeTests/` for the exact expected output.

The plugin also ships the **Phase 1 smoke tests** under the `InoAgents.*` (no `LiteRtLm.` segment) namespace — those exercise the raw LiteRT-LM C API directly, not the UE API, and exist to diagnose whether a regression is in the UE layer or the native layer. You usually don't need to run them.

---

## ElevenLabs integration (phase 1)

A standalone HTTP client for ElevenLabs' cloud audio API, completely independent of the LiteRT-LM subsystem. Phase 1 ships one endpoint:

- **Text-to-Dialogue streaming** — `POST /v1/text-to-dialogue/stream`. Synthesises multi-speaker dialogue from an array of `{voice_id, text}` inputs and streams the resulting audio back chunk by chunk.

Phases 2 (single-voice Text-to-Speech) and 3 (Speech-to-Text) are on the roadmap and will reuse the same settings / subsystem scaffolding.

Blueprint category: **`InoAgents|ElevenLabs`**. Native classes live under `Source/InoAgents/{Public,Private}/ElevenLabs/`. Nothing in this section depends on LiteRT-LM — you can use ElevenLabs without ever loading a `.litertlm` model, and vice versa.

### Configure the API key

1. Get a key at **https://elevenlabs.io/app/settings/api-keys**.
2. In the Unreal Editor, open **Edit → Project Settings → Plugins → InoAgents ElevenLabs**.
3. Paste the key into the **API Key** field (it's masked as a password) and close the dialog. The value is persisted to `Config/DefaultGame.ini` under `[/Script/InoAgents.ElevenLabsSettings]`.
4. If you edit the key mid-PIE-session, either restart PIE or run `InoAgents.ElevenLabs.ReloadSettings` in the console to re-cache it into the subsystem.

**Security note.** The key is stored **plaintext** in the ini. Do not commit `DefaultGame.ini` to a public repo once a real key is pasted in — treat it like any other dev secret. For shipping builds, pass a runtime-fetched key to the async action's `ApiKeyOverride` parameter instead of baking it into the ini.

### `UElevenLabsSettings` — Project Settings

A `UDeveloperSettings` subclass. Fields:

| Field | Type | Default | Purpose |
|---|---|---|---|
| `ApiKey` | `FString` (password-masked) | *(empty)* | xi-api-key for ElevenLabs. |
| `BaseUrl` | `FString` | *(empty → `https://api.elevenlabs.io`)* | Override for regional routing (`api.us.elevenlabs.io`, `api.eu.residency.elevenlabs.io`, `api.in.residency.elevenlabs.io`). |
| `DefaultModelId` | `FString` | `eleven_v3` | Used when a per-call request leaves `ModelId` empty. |
| `DefaultOutputFormat` | enum | `Mp3_44100_128` | Used when no explicit format is passed. |

### `UElevenLabsSubsystem` — game instance subsystem

Shared state and lifecycle anchor. One instance per game instance; access via `Get Game Instance Subsystem (ULevenLabsSubsystem)` in Blueprint or `GetGameInstance()->GetSubsystem<UElevenLabsSubsystem>()` in C++.

The subsystem caches settings at `Initialize()`, holds a UPROPERTY `TSet` of every in-flight async action (so GC can't eat them mid-request), and calls `CancelAll()` from `Deinitialize()` so HTTP responses arriving after PIE end can't land on freed UObjects. You rarely need to touch it directly — the async actions below wire themselves up automatically — but it's useful for global cancellation and for reloading settings.

| Function | Kind | Purpose |
|---|---|---|
| `ReloadSettings()` | `BlueprintCallable` | Re-reads `UElevenLabsSettings` into the cached fields without restarting PIE. Also available as the `InoAgents.ElevenLabs.ReloadSettings` console command. |
| `CancelAll()` | `BlueprintCallable` | Aborts every in-flight ElevenLabs request tracked by the subsystem. Each action receives `OnError("cancelled")` before being released. Called automatically at PIE end. |
| `GetApiKey()` / `GetBaseUrl()` / `GetDefaultModelId()` / `GetDefaultOutputFormat()` | `(C++ only)` | Cached accessors used internally by the async actions. You usually don't need to call these — bind them via the async action's per-call override parameter instead. |

### `UElevenLabsTextToDialogueStream` — latent async action

A `UBlueprintAsyncActionBase` subclass. This is the Blueprint-friendly front door for the dialogue endpoint: drag it as a single latent node with three output exec pins.

Static factory:

```cpp
UElevenLabsTextToDialogueStream::StreamTextToDialogue(
    UObject*                          WorldContextObject,
    const FElevenLabsDialogueRequest& Request,
    FString                           ApiKeyOverride);
```

Multicast delegates (all `BlueprintAssignable`, all fire on the game thread):

| Delegate | Params | Fires |
|---|---|---|
| `OnAudioChunk` | `(TArray<uint8> AudioBytes, int64 TotalBytesReceived)` | Zero or more times per request, as HTTP progress ticks land. `AudioBytes` is the **new bytes only** (not the full accumulated buffer), ready to append to your own playback buffer. |
| `OnComplete` | `(TArray<uint8> FullAudioBytes, EElevenLabsOutputFormat OutputFormat)` | Exactly once on success (2xx). `FullAudioBytes` is the complete response, byte-equal to concatenating every `OnAudioChunk` payload. |
| `OnError` | `(FString ErrorMessage)` | Exactly once on any failure path: missing API key, validation error, HTTP non-2xx, network failure, or explicit `CancelStream()`. Message is human-readable and safe to surface in UI. |

Additional method:

| Function | Kind | Purpose |
|---|---|---|
| `CancelStream()` | `BlueprintCallable` | Aborts the in-flight request. Fires `OnError("cancelled")` then destroys the action. Safe to call from any handler. No-op if the action has already finished. |

### Request types

`FElevenLabsDialogueRequest`:

| Field | Type | Default | Notes |
|---|---|---|---|
| `Inputs` | `TArray<FElevenLabsDialogueInput>` | *(empty)* | 1–10 unique voice IDs. Each input is a `{Text, VoiceId}` pair. |
| `ModelId` | `FString` | *(empty → subsystem default)* | e.g. `"eleven_v3"`. |
| `OutputFormat` | `EElevenLabsOutputFormat` | `Mp3_44100_128` | MP3 / PCM / u-law codec + sample rate. |
| `LanguageCode` | `FString` | *(empty → auto-detect)* | ISO 639-1 code. |
| `Stability` | `float` | `0.5` | Voice settings stability, clamped 0..1. |
| `Seed` | `int64` | `-1` (omit) | Deterministic sampling seed, 0..4294967295. Negative values omit the field. |
| `ApplyTextNormalization` | enum | `Auto` | `Auto` / `On` / `Off`. |

### <a id="blueprint-usage-elevenlabs"></a>Blueprint usage

Typical flow (describing nodes, not screenshots):

1. On **BeginPlay** (or button click) build an `FElevenLabsDialogueRequest`: drag off a Make struct node, populate the `Inputs` array with two-or-more `{Text, VoiceId}` entries via Make `FElevenLabsDialogueInput` nodes.
2. Drag out the **`ElevenLabs Stream Text-to-Dialogue`** node (search for "eleven" in the context menu). Hook the `WorldContextObject` pin to `self`, feed your request struct into `Request`, leave `ApiKeyOverride` empty to use the Project Settings key.
3. The node has three output exec pins that fire as events occur:
   - **`On Audio Chunk`** — fires many times. Use `Append Bytes` into a local `TArray<byte>` variable if you want to buffer, or pipe the bytes directly into your own decoder.
   - **`On Complete`** — fires once with the full buffer. Save to disk, pipe into `USoundWaveProcedural`, or hand to your own audio pipeline.
   - **`On Error`** — fires once on failure. Display the message in UI or log it.
4. Do NOT bind the output pins and then assume the node returns immediately — it IS a latent node, so execution flows out of the event pins as they fire, not the "finished" pin (there is no finished pin; each pin is terminal for its event type).

### <a id="c-usage-elevenlabs"></a>C++ usage

```cpp
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "ElevenLabs/ElevenLabsTypes.h"

FElevenLabsDialogueRequest Req;
Req.Inputs.Add({ TEXT("Knock knock."),         TEXT("JBFqnCBsd6RMkjVDRZzb") });
Req.Inputs.Add({ TEXT("Who's there?"),         TEXT("Aw4FAjKCGjjNkVhN1Xmq") });
Req.Inputs.Add({ TEXT("A plugin, streaming."), TEXT("JBFqnCBsd6RMkjVDRZzb") });
Req.OutputFormat = EElevenLabsOutputFormat::Mp3_44100_128;

UElevenLabsTextToDialogueStream* Action =
    UElevenLabsTextToDialogueStream::StreamTextToDialogue(
        /*WorldContextObject=*/ this,
        /*Request=*/            Req,
        /*ApiKeyOverride=*/     FString());  // empty = use Project Settings key

Action->OnAudioChunk.AddDynamic(this, &UMyClass::HandleChunk);
Action->OnComplete  .AddDynamic(this, &UMyClass::HandleComplete);
Action->OnError     .AddDynamic(this, &UMyClass::HandleError);
Action->Activate();   // in Blueprint this fires automatically; from C++ we call it
```

Handler signatures must match the delegate's declared parameter-passing convention exactly. In this plugin:

- **`FString`, enums, and POD/primitive types** → pass **by value**. This matches the LiteRT-LM plugin convention.
- **`TArray<T>` and other containers** → pass **`const TArray<T>&`** (by const reference). Declaring a container-returning delegate with by-value `TArray<uint8>` compiles fine but fails at Blueprint-time with "function/event does not match the necessary signature" when a user drags the latent node into a graph, because the Blueprint event-handler generator emits `const&` for containers unconditionally.

```cpp
UFUNCTION() void HandleChunk   (const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);
UFUNCTION() void HandleComplete(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat Format);
UFUNCTION() void HandleError   (FString ErrorMessage);
```

The `Action` object's lifetime is owned by `UElevenLabsSubsystem` — you do NOT need to `AddToRoot` it or store it in a `UPROPERTY` on the caller. The subsystem drops its reference once a terminal delegate fires, and GC collects the action on the next pass.

### <a id="elevenlabs-smoke-test"></a>Smoke test

Two PIE-only console commands under the `InoAgents.ElevenLabs.*` namespace:

| Command | What it does |
|---|---|
| `InoAgents.ElevenLabs.DialogueStreamTest` | Dispatches a fixed 3-line dialogue using two ElevenLabs sample voices, logs each chunk's size as it arrives, and saves the resulting audio to `Saved/InoAgents/ElevenLabs/test.mp3` (or `.pcm` / `.ulaw` depending on the default output format). Logs **PASS** and the absolute output path on success. |
| `InoAgents.ElevenLabs.ReloadSettings` | Re-reads `UElevenLabsSettings` into the subsystem's cached fields. Run this after editing the API key in Project Settings if you want to pick up the change without restarting PIE. |

Expected log output for a successful run:

```
LogInoAgents: UElevenLabsTextToDialogueStream: POST https://api.elevenlabs.io/v1/text-to-dialogue/stream?output_format=mp3_44100_128 (3 inputs, N-byte body)
LogInoAgents: DialogueStreamTest: chunk   1 (+0.412 s) — 8192 bytes (total 8192)
LogInoAgents: DialogueStreamTest: chunk   2 (+0.503 s) — 16384 bytes (total 24576)
...
LogInoAgents: DialogueStreamTest: COMPLETE — N chunks, M bytes, X.XX s
LogInoAgents: DialogueStreamTest: PASS — saved to <abs path>\Saved\InoAgents\ElevenLabs\test.mp3
```

Open the saved file in VLC / Windows Media Player — you should hear the three-line dialogue spoken by two different voices.

Failure modes:

- **`API key is empty; set it in Project Settings -> Plugins -> InoAgents ElevenLabs`** → Either you haven't set the key, or you set it mid-PIE-session but didn't run `InoAgents.ElevenLabs.ReloadSettings`.
- **`HTTP 401: ...`** → Key is invalid or expired.
- **`HTTP 402: ...`** → Key is valid but your account doesn't have access to the requested voices. The smoke test uses the two sample voices from the ElevenLabs docs (`JBFqnCBsd6RMkjVDRZzb` and `Aw4FAjKCGjjNkVhN1Xmq`); swap them out for voices from your own library if you hit this.
- **`HTTP 422: ...`** → Validation error. The message usually identifies the field; check it against `FElevenLabsDialogueRequest`.

---

## Streaming audio playback

A standalone `UAudioComponent` subclass for playing audio bytes fed in at runtime. Takes raw PCM (int16 or float32) or MP3, plays it through UE's normal audio pipeline with full volume / pitch / attenuation / spatialization / source-effect-chain / sound-class / concurrency support. **Not coupled to ElevenLabs or LiteRT-LM** — you can use it to play any audio bytes from any source (disk, HTTP, DSP pipeline, synthesis, ...).

Blueprint category: **`InoAgents|Audio`**. Native classes live under `Source/InoAgents/{Public,Private}/Audio/`. The MP3 decoder is the `minimp3` single-header library (CC0-licensed, ~1900 lines, vendored into `Private/Audio/ThirdParty/` and included from exactly one TU).

### `UInoAgentsStreamingAudioComponent` — `UAudioComponent` subclass

Drop it onto an actor via **Add Component → Streaming Audio**. Because it inherits from `UAudioComponent`, every standard audio UPROPERTY is already in the details panel:

- **Volume Multiplier** / **Pitch Multiplier**
- **Attenuation Settings** (3D falloff, distance-based volume)
- **Source Effect Chain** (low-pass filter, reverb send, custom source effects)
- **Sound Class** / **Concurrency Set**
- **bAllowSpatialization** (spatial 3D vs 2D)
- **bOverrideAttenuation** and friends

On top of that, the subclass adds byte-feeding methods and three multicast delegates:

| Method | Purpose |
|---|---|
| `SetPcmFormat(SampleRateHz, NumChannels)` | Configure the sample rate (8000–192000) and channel count (1 or 2) for the NEXT PCM stream. Must be called BEFORE the first `FeedAudioBytes`. Ignored for MP3 streams (auto-detected from frame header). Defaults: 44100 Hz mono. |
| `FeedAudioBytes(Bytes, Format)` | Append bytes to the in-flight stream. Callable many times as chunks arrive. The first call implicitly starts the stream, configures the procedural wave, and calls `Play` so audio starts playing as soon as enough bytes are queued. |
| `FinalizeStream()` | Mark the current stream as complete. After this call, the component fires `OnFinished` when the queued audio fully drains. Already-queued bytes keep playing to completion. |
| `PlayAudio(Bytes, Format)` | One-shot convenience: `FeedAudioBytes` + `FinalizeStream` in a single call. Use when you already have the entire buffer in hand. |
| `StopAndReset()` | Abort any in-flight stream, flush the queue, reset the decoder state. Does NOT fire `OnFinished` (that's reserved for the "drained cleanly after Finalize" path). |

| Delegate | Fires |
|---|---|
| `OnReadyToPlay` | Once per stream, as soon as the first bytes are queued (immediately for PCM, after the first MP3 frame decodes for MP3). Good place to trigger a "speaker speaking" UI indicator. |
| `OnFinished` | Once per stream, after `FinalizeStream` AND the queue fully drains. Triggers a tick-based polling path (cheap, one integer load per frame while active, auto-disables when idle). |
| `OnError` | Once per stream on MP3 decode failure or similar. The stream is then considered finished. |

### Format, sample rate, and channels — the three axes

This is where the API is most likely to confuse a first-time user, so read this once and it'll click.

Audio byte streams have **three independent properties** you need to tell the component about:

| Axis | Meaning | How to set |
|---|---|---|
| **Format / bit depth** | How each audio sample is encoded in the byte stream. Answers "how many bytes per sample, and what do they mean?" | The `Format` parameter on `FeedAudioBytes` / `PlayAudio` — a value from the `EInoAgentsAudioFormat` enum. |
| **Sample rate** | How many samples per second. Answers "how fast should playback be?" Typical values: 16000, 22050, 24000, 44100, 48000 Hz. | `SetPcmFormat(SampleRateHz, NumChannels)` — PCM only. MP3 auto-detects. |
| **Channel count** | Mono (1) or stereo (2). Answers "how are samples interleaved?" | `SetPcmFormat(SampleRateHz, NumChannels)` — PCM only. MP3 auto-detects. |

**Important:** `Pcm 16 kHz` and `Pcm 44 kHz` are NOT different formats — they're the same format (`PcmInt16`) played back at different sample rates. You pick the format once in the enum; you pick the sample rate separately via `SetPcmFormat`.

**The enum values:**

| Value | Meaning | Bytes per sample per channel | When to use |
|---|---|---|---|
| `PcmInt16` | Signed 16-bit integer PCM, little-endian. The universal standard for runtime audio bytes. Native input format for `USoundWaveProcedural` — no conversion happens, bytes queue straight through. | 2 | 99% of use cases. ElevenLabs PCM output, Whisper input, game audio bytes, ... |
| `PcmFloat32` | IEEE 754 single-precision float PCM. Samples are expected in the `[-1.0, +1.0]` range (out-of-range values are clamped). Converted to int16 internally before queueing. | 4 | DSP pipeline outputs that hand you float buffers, some ML/TTS libraries, VST-style audio plugins. |
| `Mp3` | MPEG-1 / 2 / 2.5 Layer III compressed audio. Sample rate and channel count are auto-detected from the first MP3 frame header — `SetPcmFormat` is ignored. Decoded on the fly by the bundled `minimp3` library. | variable | ElevenLabs default output, downloaded music files, any other MP3 source. |

**What about int8, int24, int32?**
- **int8** is obsolete (early 90s tech). If you somehow have it, upsample to int16 on your side before calling.
- **int24** is pro-audio-only (recording studios). Fiddly 3-byte packing. Not supported.
- **int32** is pro-audio-only. Not supported.
- **float64** is scientific computation only. Downcast to float32 and use `PcmFloat32`.

If you genuinely need one of these, convert to `PcmInt16` or `PcmFloat32` in your own code before calling `FeedAudioBytes`. The conversion is trivial (a few lines) and 99.9% of runtime audio sources don't need it.

**Sample rate guidance:** Match the source. If you're feeding ElevenLabs PCM 44.1 kHz bytes, call `SetPcmFormat(44100, 1)` or `(44100, 2)` depending on whether it's mono or stereo. If you're feeding 16 kHz STT audio (Whisper's native rate), call `SetPcmFormat(16000, 1)`. If you're feeding 48 kHz mixer output, call `SetPcmFormat(48000, 2)`. UE's audio mixer handles any-rate-to-any-rate conversion internally so you don't need to resample.

### <a id="blueprint-usage-audio"></a>Blueprint usage

Typical flow for playing a PCM byte array you already have in hand:

1. Drop a **Streaming Audio** component onto your actor (Add Component → Streaming Audio).
2. Adjust inherited UAudioComponent properties as desired (volume, pitch, attenuation...).
3. When ready to play: drag off the component, call **Set Pcm Format** with your sample rate and channel count, then **Play Audio** with your `TArray<byte>` buffer and `Format = PcmInt16`.
4. Optionally bind **On Ready To Play**, **On Finished**, and **On Error** events on the component to trigger UI updates.

For chunked / streaming input (e.g. from an HTTP download):

1. Drop the component onto your actor.
2. As each chunk of bytes arrives, call **Feed Audio Bytes** with `Format = PcmInt16` (or `Mp3`) — the first call starts playback automatically.
3. When the final chunk has been fed, call **Finalize Stream**.
4. `OnFinished` fires once the queue fully drains.

### <a id="c-usage-audio"></a>C++ usage

```cpp
#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "Audio/InoAgentsAudioTypes.h"

// Assume MyActor is a spawned AActor; ensure it has a component.
UInoAgentsStreamingAudioComponent* Audio =
    MyActor->FindComponentByClass<UInoAgentsStreamingAudioComponent>();

// One-shot: you have a full buffer.
Audio->SetPcmFormat(44100, /*NumChannels=*/1);
Audio->PlayAudio(MyPcmBytes, EInoAgentsAudioFormat::PcmInt16);

// Chunked: you're receiving bytes as they stream in.
Audio->SetPcmFormat(44100, 1);
for (const TArray<uint8>& Chunk : IncomingChunks)
{
    Audio->FeedAudioBytes(Chunk, EInoAgentsAudioFormat::PcmInt16);
}
Audio->FinalizeStream();

// MP3 variant: no SetPcmFormat needed (auto-detected from frame header).
Audio->PlayAudio(MyMp3Bytes, EInoAgentsAudioFormat::Mp3);

// Float32 DSP output variant:
// Samples in [-1.0, +1.0], 4 bytes per sample, mono.
Audio->SetPcmFormat(48000, 1);
Audio->PlayAudio(MyFloat32Bytes, EInoAgentsAudioFormat::PcmFloat32);
```

Bind delegates the same way you would for any other `BlueprintAssignable`:

```cpp
Audio->OnReadyToPlay.AddDynamic(this, &UMyClass::HandleAudioReady);
Audio->OnFinished.AddDynamic(this, &UMyClass::HandleAudioFinished);
Audio->OnError.AddDynamic(this, &UMyClass::HandleAudioError);
```

Handler signatures:

```cpp
UFUNCTION() void HandleAudioReady();
UFUNCTION() void HandleAudioFinished();
UFUNCTION() void HandleAudioError(FString ErrorMessage);
```

### ElevenLabs → audio component wiring

This is the headline use case: TTS dialogue audible in-game, live, with zero disk intermediary.

**Blueprint setup:**

1. Drop a **Streaming Audio** component on any actor.
2. In that actor's BP, add the **ElevenLabs Stream Text-to-Dialogue** latent node.
3. Wire the node's **On Audio Chunk** event to **Feed Audio Bytes** on the component (set `Format = Mp3`).
4. Wire **On Complete** to **Finalize Stream**.
5. Wire **On Error** to **On Error** on the component (or your own error handler).
6. Press Play. Audio plays from the actor's 3D position as it streams in from ElevenLabs.

No sample-rate configuration is needed because MP3 carries that metadata in every frame. If you switch ElevenLabs' output format to PCM (via Project Settings → Plugins → InoAgents ElevenLabs → Default Output Format), update the Blueprint wiring to pass `Format = PcmInt16` and call `SetPcmFormat(44100, 1)` (or whichever rate you selected) before the first `OnAudioChunk`.

### <a id="streaming-audio-smoke-tests"></a>Smoke tests

Three PIE-only console commands under `InoAgents.Audio.*`:

| Command | What it does |
|---|---|
| `InoAgents.Audio.PlayPcmTest` | Generates a 1-second 440 Hz sine wave at 44100 Hz mono int16, plays it via `PlayAudio`, verifies `OnReadyToPlay` and `OnFinished` both fire. Exercises the PCM path end-to-end with no decoder involvement. |
| `InoAgents.Audio.PlayMp3Test [path]` | Loads an MP3 file from disk and plays it in one shot. Defaults to `Saved/InoAgents/ElevenLabs/test.mp3` so it plays whatever the ElevenLabs smoke test most recently generated. Optional path argument to override. |
| `InoAgents.Audio.PlayMp3ChunkedTest [path]` | Same MP3 file, but sliced into 4 KB chunks fed one per 50 ms via `FTSTicker` + `FeedAudioBytes`, then `FinalizeStream()`. Proves playback starts before all chunks are delivered. |

Expected flow for the full smoke test run:

```
# 1. Run the ElevenLabs smoke test to produce a test.mp3:
InoAgents.ElevenLabs.DialogueStreamTest

# 2. Sanity-check the PCM path (synthesised, no file needed):
InoAgents.Audio.PlayPcmTest
#    -> 1 second of 440 Hz tone audible from the editor listener

# 3. Play back what ElevenLabs just generated:
InoAgents.Audio.PlayMp3Test
#    -> multi-line dialogue audible

# 4. Prove chunked ingestion works:
InoAgents.Audio.PlayMp3ChunkedTest
#    -> same dialogue, but the log shows chunk-by-chunk feed while
#       audio is already playing
```

---

## Known limitations

- **Windows Win64 only.** Android, iOS, Linux, and macOS phases are planned — the Bazel build already has configs for all four, only the `InoAgentsLibrary.Build.cs` branches need porting.
- **CPU inference only in the current shipped Bazel target.** The build can be swapped to `//c:engine` to enable GPU (D3D12), but that target has not been validated against the Milestone D surface yet.
- **One active conversation per engine at a time.** LiteRT-LM appears to reject creating a second native conversation while a prior one is still alive. Running multiple conversations back-to-back requires calling `Shutdown()` on the previous one (or letting GC reclaim it) before creating the next. A future milestone will lift this to true parallel conversations.
- **Deferred tool results are stubbed.** `SubmitDeferredToolResult` exists in the header so Blueprint code can wire it up, but the worker-side state machine that would consume async tool results is not implemented yet. All current tools must be synchronous (game-thread `Execute` returns a result immediately).
- **Tool calling is reliable on Gemma family models only.** The `libGemmaModelConstraintProvider.dll` that makes constrained decoding work is Gemma-specific. Non-Gemma models load and chat fine but tool calls may produce unreliable JSON.
- **Gemma 4 E2B sometimes ignores tools and computes inline.** Small models are small. Steering the model with a very direct system message (`"You MUST call the add_numbers tool ..."`) and tool descriptions (`"Always use this tool when ..."`) moves the needle significantly, but don't expect 100% tool-use rates on the smallest variant.
- **ElevenLabs integration is phase 1 only.** Only `/v1/text-to-dialogue/stream` is wired up today. Single-voice Text-to-Speech (`/v1/text-to-speech/{voice_id}/stream`) and Speech-to-Text (`/v1/speech-to-text`) are planned for phases 2 and 3 respectively.
- **ElevenLabs delivers raw audio bytes, not a `USoundWave`.** The plugin hands you the full `TArray<uint8>` in `OnComplete` (and incrementally in `OnAudioChunk`). Decoding MP3 / PCM / u-law into something UE's audio engine can play is the caller's responsibility — a future phase may add a built-in helper that constructs a `USoundWaveProcedural` from the buffer.
- **ElevenLabs API key is stored plaintext in `DefaultGame.ini`.** Do not commit the ini with a real key. For shipping builds, pass a runtime-fetched key via the async action's `ApiKeyOverride` parameter instead.
- **ElevenLabs rate-limit / retry handling is not built in.** 4xx / 5xx responses fire `OnError` with the server's message; the caller decides whether to retry and when. No exponential backoff, no queueing.
- **Streaming audio component supports PCM int16, PCM float32, and MP3 only.** Opus, Vorbis, u-law, A-law, and other formats are not decoded. If you need them, convert in your own code first, or open an issue. Obsolete PCM variants (int8, int24, int32) are deliberately not exposed — use int16 or float32 and convert on your side if strictly necessary.
- **Streaming audio MP3 decoding runs on the game thread.** `minimp3` is very fast (one frame decodes well under a millisecond) so this is fine for TTS-sized buffers. Long music streams would benefit from a worker-thread decoder — not yet implemented.
- **Streaming audio component does NOT save USoundWave assets.** It plays bytes directly through a `USoundWaveProcedural` that lives only as long as the component. To persist audio, save the bytes to disk yourself from your `OnComplete` (for ElevenLabs) or `OnFinished` handler.

---

## Further reading

- **`CLAUDE.md`** — architecture, threading model, Bazel build notes, Windows gotchas, model file distribution, tool-calling flow. Read this if you are modifying the plugin itself.
- **`docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md`** — the Milestone D design record. Written before implementation started; read alongside the code for the why behind each decision.
- **`docs/superpowers/plans/2026-04-11-milestone-d-litert-lm-ue-api.md`** — the Milestone D implementation plan with full inline code for D.1 and D.2.
- **LiteRT-LM upstream** — https://github.com/google-ai-edge/LiteRT-LM
- **Gemma 4 edge models on Hugging Face** — https://huggingface.co/litert-community
