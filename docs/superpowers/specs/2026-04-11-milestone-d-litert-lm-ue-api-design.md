# Milestone D — LiteRT-LM UE API Design

**Status:** accepted (pending user review of this file)
**Date:** 2026-04-11
**Author:** brainstormed collaboratively with Claude during the phase-1 → phase-2 transition
**Implements:** milestone D of the InoAgents plugin phase plan

---

## 1. Context

Phase 1 of the InoAgents plugin proved the full native integration with LiteRT-LM end-to-end through five console smoke tests: engine load, raw generation, chat-template conversation, tool calling with an `add_numbers` tool, and non-blocking streaming with worker-thread → game-thread marshaling. Every link in the chain from Bazel build output to UE editor invocation is verified.

What does NOT yet exist is the **UE-facing API** that designers and gameplay programmers will actually use. Today the only way to talk to LiteRT-LM from UE is via the native C API directly from C++ inside the smoke test files. There are no UObjects, no Blueprint bindings, no delegates, no lifetime management, no tool registry. Milestone D builds that layer.

This milestone is specifically the **LiteRT-LM backend** of the InoAgents plugin. The plugin is intended to host multiple backends in the future (OpenAI API, Anthropic API, llama.cpp, etc.). Each backend gets its own subdirectory, its own subsystem, its own set of classes. No shared abstract interface is extracted in this milestone — that would be premature abstraction before we have a second backend to compare against.

## 2. Scope

### In scope

- A `UGameInstanceSubsystem` that owns the loaded `LiteRtLmEngine*` and a tool registry.
- A `UDataAsset` for designer-editable model configuration (which `.litertlm` file, which backend, which system message, etc.).
- A `UObject`-based conversation wrapper with Blueprint-bindable multicast delegates for streaming tokens, completion, errors, and tool-call diagnostics.
- A `UInterface` for implementing tools in C++ or Blueprint, with synchronous execution on the game thread.
- Worker-thread isolation per conversation, with all Blueprint-visible callbacks marshaled to the game thread.
- Four new console smoke tests, one per sub-milestone, that exercise the new UE API through its intended public surface.

### Out of scope (explicitly deferred)

- **Blueprint latent nodes.** We use delegates only. Latent nodes can be added later as ergonomic wrappers around the delegate API once the delegates are known to work.
- **Asynchronous tool execution.** `ILiteRtLmTool::Execute` returns synchronously on the game thread. A deferred-result API (`SubmitDeferredToolResult`) is stubbed into the interface for future-proofing but not wired through Milestone D's agent loop.
- **Multi-modal input.** Text only. Image and audio inputs need additional `InputData` plumbing; that is a later milestone.
- **Model hot-swapping.** `LoadModelAsync` + `UnloadModel` are exposed, but we do not optimize for switching between models at runtime. Assume one model per game session.
- **Conversation history serialization.** In-progress agent conversations do not survive save-game. A conversation exists while the `ULiteRtLmConversation` exists.
- **Network replication of conversations.** Conversations are local-client-only. Multiplayer agent interaction is a later design question.
- **Telemetry / benchmark APIs.** LiteRT-LM exposes `litert_lm_benchmark_info_*` functions, but we do not surface them at the UE layer yet.
- **Retrying `LoadModelAsync` while a load is in flight.** Explicitly rejected; logs an error.
- **Multiple simultaneously-loaded models.** One model at a time per game instance. Switching requires `UnloadModel` followed by a new `LoadModelAsync`.
- **A shared abstract backend interface** (e.g. `IInoAgentsBackend`). Not extracted until there are two concrete backends to compare.

## 3. Architecture at a glance

### Layer summary

| Layer | Class | UE primitive | Role |
|---|---|---|---|
| Global | `ULiteRtLmSubsystem` | `UGameInstanceSubsystem` | Owns the loaded engine + tool registry. One per game instance. |
| Config | `ULiteRtLmModelConfig` | `UDataAsset` | Designer-editable model pick. Created in the Content Browser. |
| Conversation | `ULiteRtLmConversation` | `UObject` | One stateful chat with the model. Multiple can exist in parallel. |
| Tool | `ILiteRtLmTool` / `ULiteRtLmTool` | `UInterface` | Contract for implementing tools. Implementors provide name, schema, execute. |
| Types | `ELiteRtLmBackend`, delegates | enum + `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` | Shared type definitions. |

### Why these four classes and not fewer / more

- The engine is **expensive**, **global**, and **shared**. It belongs on the subsystem.
- Conversations are **cheap**, **many**, and **independent**. They belong on a per-instance UObject.
- Tools are **pluggable** — designers write tools in Blueprint without touching C++. A `UInterface` is the only UE construct that lets Blueprint classes natively implement a contract.
- Model config is **designer-editable** and must persist as an asset. A `UDataAsset` is the canonical UE way.

Collapsing any of these into another would produce one of:
- A single object with two lifetimes (broken — engine is game-long, conversations are per-chat),
- A C++-only API (broken — designers can't add tools without recompiling),
- Global functions (broken — no way to have multiple simultaneous conversations).

The four-way split falls out of the actual constraints. It is not aesthetic.

## 4. File organization

```
Source/InoAgents/
├── InoAgents.Build.cs                         (+ PrivateIncludePaths for LiteRtLm subdir)
├── Public/
│   ├── InoAgents.h                            (framework module header, unchanged)
│   └── LiteRtLm/                              (LiteRT-LM backend — this milestone)
│       ├── LiteRtLmTypes.h                    (enum ELiteRtLmBackend, delegates)
│       ├── LiteRtLmModelConfig.h
│       ├── LiteRtLmSubsystem.h
│       ├── LiteRtLmConversation.h
│       └── LiteRtLmTool.h
└── Private/
    ├── InoAgents.cpp                          (unchanged from refactor — module lifecycle only)
    ├── InoAgentsLog.h                         (unchanged — shared log category)
    ├── LiteRtLm/
    │   ├── LiteRtLmModelConfig.cpp            (probably empty; UDataAsset needs no body)
    │   ├── LiteRtLmSubsystem.cpp
    │   ├── LiteRtLmConversation.cpp
    │   ├── LiteRtLmConversationWorker.h       (private FRunnable; not in Public/)
    │   ├── LiteRtLmConversationWorker.cpp
    │   └── LiteRtLmTool.cpp                   (default interface implementations only)
    └── SmokeTests/                            (existing phase-1 tests untouched)
        ├── InoAgentsLiteRtLmSubsystemLoadTest.cpp    (D.1)
        ├── InoAgentsLiteRtLmConversationSendTest.cpp (D.2)
        ├── InoAgentsLiteRtLmConversationStreamTest.cpp (D.3)
        └── InoAgentsLiteRtLmConversationToolTest.cpp (D.4)
```

Future shape, for reference only (not part of this milestone):

```
Source/InoAgents/Public/
├── LiteRtLm/        ← this milestone
├── OpenAI/          ← future backend
└── Anthropic/       ← future backend
```

Each backend gets its own subdirectory. Nothing in this milestone creates a shared base class; that decision is explicitly deferred until a second backend exists.

`Source/InoAgents/InoAgents.Build.cs` gains one new entry in `PrivateIncludePaths` for the `LiteRtLm` Private subdirectory so that `.cpp` files there can `#include "LiteRtLmConversationWorker.h"` with a plain include.

## 5. Component specs

### 5.1 `ULiteRtLmModelConfig`

```cpp
// LiteRtLmModelConfig.h
UCLASS(BlueprintType)
class INOAGENTS_API ULiteRtLmModelConfig : public UDataAsset
{
    GENERATED_BODY()
public:
    /**
     * Filename of the .litertlm model file, resolved relative to the plugin's
     * Models/ directory. Example: "gemma-4-E2B-it.litertlm".
     *
     * Not a full path — the subsystem prepends
     *   IPluginManager::FindPlugin("InoAgents")->GetBaseDir() + "/Models/".
     * This keeps configs portable across machines.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM")
    FString ModelFileName = TEXT("gemma-4-E2B-it.litertlm");

    /**
     * Which backend the engine uses. Maps to LiteRT-LM's backend string.
     *   Cpu → "cpu"
     *   Gpu → "gpu" (on Windows: D3D12 via libLiteRtWebGpuAccelerator.dll)
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM")
    ELiteRtLmBackend Backend = ELiteRtLmBackend::Cpu;

    /**
     * Upper bound on tokens per decode step. Zero means "use engine default".
     * Only meaningful for some backends.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM",
              meta=(ClampMin="0"))
    int32 MaxNumTokens = 0;

    /**
     * Optional system message applied to every conversation created with this
     * config as the template. Plain text; the subsystem wraps it in the
     * expected JSON shape before handing it to LiteRT-LM.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM",
              meta=(MultiLine=true))
    FString SystemMessage;

    /**
     * NOTE: tool calling via constrained decoding is Gemma-family-only today
     * because libGemmaModelConstraintProvider.dll is shipped only for Gemma
     * models. Pointing this config at a non-Gemma model (Qwen, generic, etc.)
     * will work for chat but may produce unreliable tool calling.
     */
};
```

Rationale: keeps "which model, which backend, what system message" in designer-editable assets, out of C++. The `MaxNumTokens` default of `0` is a sentinel meaning "don't set it, use engine default" — this avoids committing to a specific non-zero default that might be wrong for different models.

### 5.2 `ULiteRtLmSubsystem`

```cpp
// LiteRtLmSubsystem.h
UCLASS()
class INOAGENTS_API ULiteRtLmSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    //~ UGameInstanceSubsystem
    // Initialize: cheap. Just zero-inits members. No engine load here —
    // the editor would freeze on startup.
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    // Deinitialize: internally calls UnloadModel() and UnregisterTool for
    // every registered tool, in that order. Runs on the game thread at
    // game-instance shutdown. If any conversations are still alive, see
    // the UnloadModel contract below.
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem

    // ---------------------------------------------------------------------
    // Model lifecycle
    // ---------------------------------------------------------------------

    /**
     * Asynchronously load a LiteRT-LM engine from the given model config.
     * Returns immediately. When loading finishes (successfully or otherwise),
     * OnLoaded fires on the game thread.
     *
     * Calling this while a load is already in flight, or while a model is
     * already loaded, is an error: logs a warning and fires OnLoaded with
     * bSuccess=false and a descriptive ErrorMessage.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM",
              meta=(AutoCreateRefTerm="OnLoaded"))
    void LoadModelAsync(
        const ULiteRtLmModelConfig* Config,
        const FOnLiteRtLmModelLoaded& OnLoaded);

    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsModelLoaded() const;

    /**
     * Destroy the loaded engine. Safe to call even if no model is loaded.
     * If any conversations are still alive, they will stop working — the
     * caller is responsible for tearing down conversations before unloading.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void UnloadModel();

    // ---------------------------------------------------------------------
    // Conversation factory
    // ---------------------------------------------------------------------

    /**
     * Create and return a new conversation bound to the currently loaded
     * engine. Returns nullptr if no model is loaded.
     *
     * The subsystem does NOT own the returned conversation. The caller must
     * hold a reference (UPROPERTY on an actor, widget, or other UObject) to
     * keep it alive. When no references remain, UE garbage collection will
     * destroy the conversation, which will in turn join its worker thread
     * and release native resources.
     *
     * The optional OverrideConfig parameter lets the caller use a different
     * system message / sampler params than the subsystem's loaded config,
     * without unloading and reloading the engine.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    ULiteRtLmConversation* CreateConversation(
        const ULiteRtLmModelConfig* OverrideConfig = nullptr);

    // ---------------------------------------------------------------------
    // Tool registry
    // ---------------------------------------------------------------------

    /**
     * Register a tool implementation so it becomes available to every
     * conversation created after registration. Re-registering under the
     * same tool name overwrites the previous registration.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void RegisterTool(TScriptInterface<ILiteRtLmTool> Tool);

    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void UnregisterTool(FName ToolName);

    // ---------------------------------------------------------------------
    // Internal — not Blueprint-exposed
    // ---------------------------------------------------------------------

    /** Returns the tool registered under the given name, or nullptr. */
    TScriptInterface<ILiteRtLmTool> FindTool(FName ToolName) const;

    /**
     * Serialize the currently registered tools into the JSON shape
     * LiteRT-LM expects for litert_lm_conversation_config_create's
     * `tools_json` parameter. Returns an empty string if no tools are
     * registered (causes conversations to skip the `tools_json` arg).
     */
    FString BuildToolsJsonForConversation() const;

    /** Access the raw engine for conversation construction. Internal only. */
    struct LiteRtLmEngine* GetEngineNative() const { return Engine; }

private:
    // Opaque native handles. Not BLueprint-visible, not in UPROPERTY.
    LiteRtLmEngine*          Engine   = nullptr;
    LiteRtLmEngineSettings*  Settings = nullptr;

    UPROPERTY()
    TObjectPtr<const ULiteRtLmModelConfig> LoadedConfig;

    UPROPERTY()
    TMap<FName, TScriptInterface<ILiteRtLmTool>> Tools;

    bool bLoadInFlight = false;
};
```

**Rationale points worth spelling out:**

- **Why `FOnLiteRtLmModelLoaded` is a single-cast dynamic delegate, not a multicast:** loading a model is a one-shot operation per `LoadModelAsync` call. The caller attaches exactly one completion handler when they invoke the load. Multicast would imply multiple observers, which is not the right model for an async factory call.
- **Why the subsystem does not own conversations:** giving the subsystem ownership would require reference counting to decide when to destroy a conversation, plus an API for the caller to "release" the conversation. Having the caller hold a `UPROPERTY` reference uses UE's existing garbage collection for free and matches how `UUserWidget` and similar are used.
- **Why `GetEngineNative` is public but unexposed to Blueprint:** the `ULiteRtLmConversation` constructor needs the engine pointer to create the underlying `LiteRtLmConversation*`, and `ULiteRtLmConversation` is not a friend class (UE's reflection + template machinery makes friend classes awkward). Exposing the accessor at C++ level is the simplest correct option. Not putting it in a `UFUNCTION` keeps it out of Blueprint's field of view.

### 5.3 `ULiteRtLmConversation`

```cpp
// LiteRtLmConversation.h
UCLASS(BlueprintType)
class INOAGENTS_API ULiteRtLmConversation : public UObject
{
    GENERATED_BODY()
public:
    virtual void BeginDestroy() override;

    // ---------------------------------------------------------------------
    // Entry points
    // ---------------------------------------------------------------------

    /**
     * Send a user message to the conversation. Returns immediately; the
     * actual inference happens on this conversation's dedicated worker
     * thread. Results arrive asynchronously via the delegates below.
     *
     * Calling SendMessageAsync while a previous send is still in flight
     * enqueues the new message — it will be processed once the current
     * one completes (or fails, or is cancelled). No message is ever lost.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SendMessageAsync(const FString& UserText);

    /**
     * Request cancellation of whatever is currently in flight. Safe to
     * call at any time. If no send is active, this is a no-op.
     *
     * Cancellation is best-effort: the model may still emit one or two
     * more tokens after the cancel request before actually stopping.
     * When cancellation completes, OnError fires with ErrorMessage
     * "cancelled by user".
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Cancel();

    /**
     * Advanced path for tools that want to return their result
     * asynchronously rather than inline from Execute(). Not used by
     * Milestone D's test tools. Stubbed for future expansion.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SubmitDeferredToolResult(FName ToolCallId, const FString& ResultJson);

    // ---------------------------------------------------------------------
    // Delegates (multicast, Blueprint-bindable)
    // ---------------------------------------------------------------------

    /**
     * Fires once per streaming chunk. Blueprint code typically binds a
     * UMG text widget to this and appends chunks as they arrive. Always
     * fires on the game thread.
     *
     * Chunks are typically single tokens or single subword pieces. A
     * chunk may be a bare newline token — emit it verbatim; do not
     * trim or normalize.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmToken OnToken;

    /**
     * Fires exactly once per SendMessageAsync call after all tokens have
     * been delivered (and any tool calls have been handled). FullText
     * is the concatenation of every chunk this send emitted, with no
     * surrounding whitespace trimming.
     *
     * Does NOT fire if OnError fired. Either OnComplete or OnError
     * fires for each send, never both, and exactly one of them.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmComplete OnComplete;

    /**
     * Fires exactly once per SendMessageAsync call in the error case.
     * Does not fire if OnComplete fired.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmError OnError;

    /**
     * Diagnostic event fired whenever the model emits a tool call that
     * the conversation handles internally via the subsystem's tool
     * registry. Fires AFTER the tool has been executed and its result
     * fed back into the conversation, so ResultJson is populated.
     *
     * Most Blueprint graphs do NOT need to bind this — it is purely
     * observational, for debug UI and logging. The tool call itself is
     * completely handled inside the conversation.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmToolCalled OnToolCalled;

    // ---------------------------------------------------------------------
    // Internal — constructed by ULiteRtLmSubsystem::CreateConversation
    // ---------------------------------------------------------------------

    void Initialize(
        ULiteRtLmSubsystem* InSubsystem,
        LiteRtLmEngine* InEngine,
        const ULiteRtLmModelConfig* InConfig);

private:
    UPROPERTY()
    TWeakObjectPtr<ULiteRtLmSubsystem> Subsystem;

    // Worker that owns all interaction with the native LiteRtLmConversation.
    // Lives on its own pinned FRunnableThread. Not Blueprint-visible.
    TUniquePtr<FLiteRtLmConversationWorker> Worker;
};
```

**Rationale points:**

- **Why delegates and not Blueprint latent nodes:** latent nodes are more ergonomic for single-response "call and wait" patterns. We explicitly want **streaming + multi-observer**. Multicast delegates are the canonical UE way. Latent wrappers can be added later on top.
- **Why `TWeakObjectPtr` for the subsystem reference:** if the game instance is shutting down (e.g. PIE session ending), the subsystem may be garbage-collected before all outstanding conversations are. A weak pointer lets the conversation's worker thread notice this during final cleanup and skip any tool-registry lookups that would otherwise dereference a dangling subsystem.
- **Why the worker is `TUniquePtr<FLiteRtLmConversationWorker>` and not a `UObject` member:** the worker is a `FRunnable`, not a `UObject`. `TUniquePtr` is the correct ownership for plain C++ objects, and destruction in the `ULiteRtLmConversation` destructor will join the worker thread before freeing native resources.

### 5.4 `ILiteRtLmTool`

```cpp
// LiteRtLmTool.h
UINTERFACE(BlueprintType, Blueprintable)
class INOAGENTS_API ULiteRtLmTool : public UInterface
{
    GENERATED_BODY()
};

class INOAGENTS_API ILiteRtLmTool
{
    GENERATED_BODY()
public:
    /**
     * Unique identifier used by the model when emitting a tool call.
     * Must match the "name" field in the JSON schema returned by
     * GetToolSchemaJson.
     *
     * Convention: snake_case, ASCII. Example: "get_player_health".
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM")
    FName GetToolName() const;

    /**
     * OpenAI-style function schema as a JSON string. Returned verbatim
     * in the subsystem's BuildToolsJsonForConversation() array.
     *
     * Example:
     *   {
     *     "type": "function",
     *     "function": {
     *       "name": "get_player_health",
     *       "description": "Returns the current HP of the player character.",
     *       "parameters": {
     *         "type": "object",
     *         "properties": {},
     *         "required": []
     *       }
     *     }
     *   }
     *
     * The tool's own "name" field MUST match GetToolName() or the
     * subsystem will log a warning and skip registration.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM")
    FString GetToolSchemaJson() const;

    /**
     * Execute the tool. ArgumentsJson is a JSON object matching the
     * parameters defined in the schema. Return a JSON scalar or object
     * with the result.
     *
     * Called on the GAME THREAD. Implementors may freely call any UE
     * API (GetActorLocation, Cast<ACharacter>, etc.) without thread
     * safety concerns.
     *
     * Errors should be returned as a JSON object with an "error" field:
     *     {"error": "player not found"}
     * Do NOT throw exceptions. The conversation forwards the returned
     * string to the model as a tool result regardless of whether it
     * encodes success or failure, and the model is responsible for
     * handling error responses.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM")
    FString Execute(const FString& ArgumentsJson);
};
```

**Rationale:**

- **`BlueprintNativeEvent`** means a C++ subclass can override `Execute_Implementation` AND a Blueprint class can implement `Execute` as a graph. Both paths work from day one.
- **No out-parameters, no status codes, no exceptions.** The "errors come back as `{"error":...}`" convention keeps the contract minimal and self-describing. It is also how OpenAI's function-calling API works, so model behavior on error responses is well-trained.
- **Synchronous by default.** Async tools are a known-valuable feature, but they complicate the agent loop considerably. Stubbing `SubmitDeferredToolResult` in the conversation API reserves the design space for later without forcing it into Milestone D.

### 5.5 Shared types

```cpp
// LiteRtLmTypes.h
#pragma once
#include "CoreMinimal.h"
#include "LiteRtLmTypes.generated.h"

UENUM(BlueprintType)
enum class ELiteRtLmBackend : uint8
{
    Cpu  UMETA(DisplayName="CPU"),
    Gpu  UMETA(DisplayName="GPU (D3D12/WebGPU)"),
};

/** Maps ELiteRtLmBackend to the C string LiteRT-LM expects. */
INOAGENTS_API const char* LiteRtLmBackendToString(ELiteRtLmBackend Backend);

// ---------------------------------------------------------------------
// Delegates
// ---------------------------------------------------------------------

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnLiteRtLmModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmToken,
    FString, Chunk);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmComplete,
    FString, FullText);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmError,
    FString, ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnLiteRtLmToolCalled,
    FName, ToolName,
    FString, ArgumentsJson,
    FString, ResultJson);
```

## 6. Threading model

Non-negotiable rules:

1. **Game thread owns all UObject access and delegate broadcasts.** Blueprint code never sees a non-game-thread invocation.
2. **Each `ULiteRtLmConversation` has one dedicated worker thread** (`FRunnable` on a pinned `FRunnableThread`). The worker owns all interaction with the native `LiteRtLmConversation*`. LiteRT-LM conversations are stateful and not thread-safe; one worker per conversation is the canonical isolation.
3. **Worker → game thread marshaling via `AsyncTask(ENamedThreads::GameThread, ...)`** — the pattern we proved in Phase 1 milestone C. No shared mutable state, no locks, no condition variables visible to Blueprint.
4. **Tool `Execute` runs on the game thread.** The conversation's worker thread enqueues a game-thread task to invoke the tool, blocks on a `TPromise<FString>`, resumes when the result arrives. Tool authors can freely touch UE actors and components without thread-safety concerns. The game thread itself is never blocked because the conversation's `SendMessageAsync` is already async.
5. **Conversation destruction on the game thread joins the worker thread before destroying native resources.** `BeginDestroy` sets a cancellation flag, joins the worker, then destroys the native `LiteRtLmConversation*`. No dangling worker callbacks, no `AsyncTask` firing into a freed `ULiteRtLmConversation*`. Any `AsyncTask` lambda captured before destruction must check a `TWeakObjectPtr` and no-op if the target is gone.

### Data flow — the agent loop

```
[ Game code or Blueprint ]                     [ Worker thread ]
       |                                             |
       |  Conversation = Subsystem.CreateConversation() |
       |  Conversation.OnToken.AddDynamic(W, F)      |
       |  Conversation.SendMessageAsync("...")       |
       |  (returns immediately)                      |
       |                                             |
       |  enqueues work ─────────────────────────>   |
       |                                             |  calls LiteRT-LM
       |                                             |  (model generates tokens)
       |                                             |    ↓
       |   <── AsyncTask(GameThread, chunks) ─────── |
       v                                             |
[ OnToken "Checking" ] [ OnToken " player" ]         |
                                                     |
       ... (model emits a tool call)                 |
                                                     |
       |   <── AsyncTask(GameThread,                 |
       |          execute tool "get_player_health") ─|
       v                                             |
[ Lookup tool in subsystem registry ]                |
[ ILiteRtLmTool::Execute("{}") on game thread ]      |
[ Returns "{\"hp\":87}" ]                            |
[ Game-thread lambda fulfils worker's TPromise ]     |
       |                                             |
       |  tool result ──────────────────────────>    |
       |                                             |  feeds back into LiteRT-LM
       |                                             |  (model continues)
       |   <── more tokens ────────────────────────  |
       v                                             |
[ OnToken "87 HP." ]  [ OnComplete "Player HP is 87 HP." ]
```

## 7. Error handling

- **`LoadModelAsync` failure** → `OnLoaded(false, ErrorMessage)`. Never throws, never returns failure synchronously. `IsModelLoaded()` stays false.
- **`CreateConversation` with no model loaded** → returns `nullptr`. Caller is responsible for a null check.
- **`SendMessageAsync` — model returns NULL, tool lookup failed, JSON parse failed on response** → `OnError(ErrorMessage)`. `OnComplete` does not fire. Exactly one of the two fires per send.
- **`ILiteRtLmTool::Execute` — implementor returns an error JSON object** → the conversation forwards it to the model verbatim. The model sees the error as a tool response and is responsible for recovering (usually by explaining the failure to the user).
- **`ILiteRtLmTool::Execute` — implementor throws** → caught by the conversation, converted to `{"error":"<exception type>: <what()>"}`, forwarded to the model. The conversation does not let exceptions propagate out of the game-thread task.
- **`Cancel` during active send** → `OnError("cancelled by user")`. Any partial output is discarded.
- **Conversation destroyed during active send** → the cancellation flag is raised, the worker joins, native resources are freed. No delegate fires (because the UObject is dying).
- **AsyncTask lambda fires after `ULiteRtLmConversation` destruction** → the lambda captures a `TWeakObjectPtr` and checks validity; on invalid, it no-ops. No use-after-free.
- **Duplicate tool registration** → last-registered wins. The subsystem logs an info-level message noting the replacement.
- **Tool schema's `"name"` field does not match `GetToolName()`** → subsystem logs a warning and refuses to register the tool.

## 8. Sub-milestone breakdown

Each sub-milestone is a testable commit. Implementation follows the order D.1 → D.2 → D.3 → D.4. Each sub-milestone is committed when its smoke test passes.

### D.1 — Subsystem skeleton + async model load

**What this delivers:** `ELiteRtLmBackend`, `FOnLiteRtLmModelLoaded`, `ULiteRtLmModelConfig`, `ULiteRtLmSubsystem` (partial: only `Initialize`, `Deinitialize`, `LoadModelAsync`, `IsModelLoaded`, `UnloadModel`). No conversation, no tools.

**Internal machinery:** the async load uses `Async(EAsyncExecution::ThreadPool, ...)` with a `TFuture<TPair<LiteRtLmEngine*, FString>>` whose `.Then` callback fires on the game thread to invoke the `FOnLiteRtLmModelLoaded` delegate. No dedicated worker thread yet — model load is a one-shot operation that doesn't need a persistent worker.

**New file:** `Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.cpp` registering `InoAgents.LiteRtLm.SubsystemLoadTest`. The test constructs a `ULiteRtLmModelConfig` inline in C++ (not from an asset), calls `LoadModelAsync`, waits on the delegate, logs success + duration, then calls `UnloadModel`.

**Success criteria:** the test prints `SubsystemLoadTest: model loaded in X.XX s` on the game thread without freezing the editor.

### D.2 — Conversation + non-streaming send

**What this delivers:** `ULiteRtLmConversation` with `SendMessageAsync`, `OnComplete`, `OnError`, and `BeginDestroy`. No streaming, no `OnToken`, no tools. `FLiteRtLmConversationWorker` is introduced as a `FRunnable` that owns the native `LiteRtLmConversation*` and uses the blocking `litert_lm_conversation_send_message` internally. The worker writes the full response into a single `OnComplete` via `AsyncTask`.

**Why start with non-streaming:** streaming has moving parts (per-chunk callback, accumulator state, chunk marshaling, final-chunk cleanup). Splitting that out lets D.2 focus on proving the worker-thread + conversation lifetime pattern in isolation.

**New file:** `Private/SmokeTests/InoAgentsLiteRtLmConversationSendTest.cpp` registering `InoAgents.LiteRtLm.ConversationSendTest`. The test loads the model via subsystem, creates a conversation with a system message, binds `OnComplete`, sends `"What is 2 plus 2?"`, logs the full text when it arrives, destroys the conversation, unloads the model.

**Success criteria:** prints the final assistant text on the game thread; the test returns without freezing the editor; no crash on conversation/worker destruction.

### D.3 — Streaming

**What this delivers:** the conversation worker switches from `litert_lm_conversation_send_message` to `litert_lm_conversation_send_message_stream` with a C callback. The callback accumulates chunks into `FString`s on the worker thread, marshals per-chunk `OnToken` broadcasts to the game thread, and on the final chunk marshals a single `OnComplete` broadcast. Adds `Cancel` (sets an atomic flag checked by the worker).

**What stays the same from D.2:** the conversation's public API, the subsystem's creation path, the smoke test structure. Streaming is a swap of the internal generation call plus wiring `OnToken`.

**New file:** `Private/SmokeTests/InoAgentsLiteRtLmConversationStreamTest.cpp` registering `InoAgents.LiteRtLm.ConversationStreamTest`. The test creates a conversation, binds `OnToken` (to log each chunk) and `OnComplete` (to log the full text), sends a short completion prompt, and observes chunks arriving asynchronously.

**Success criteria:** chunks appear in the Output Log one at a time without freezing the editor; `OnComplete` fires with the concatenated full text exactly once.

### D.4 — Tool calling

**What this delivers:** `ILiteRtLmTool` interface, subsystem `RegisterTool` / `UnregisterTool` / `BuildToolsJsonForConversation`, and the conversation's agent loop that detects tool-call responses and round-trips them through registered tools. The default-provided `ULiteRtLmAddNumbersTool` C++ class implements `ILiteRtLmTool` for the smoke test.

**How the agent loop works internally:**

1. Worker sends the user message via `litert_lm_conversation_send_message_stream`.
2. When a chunk arrives that indicates a tool call (from LiteRT-LM's constrained decoding output), the worker parses the tool call JSON and queues a game-thread task to invoke the tool.
3. The game-thread task looks up the tool in the subsystem's registry (via `TWeakObjectPtr<ULiteRtLmSubsystem>`), invokes `Execute(ArgumentsJson)`, fulfils the worker's `TPromise<FString>` with the result.
4. The worker resumes, sends the tool result back to the conversation as a `role:"tool"` message, and waits for the follow-up streaming response.
5. Eventually the model emits its final text response, `OnComplete` fires with the concatenated text (the tool-call-and-response interlude is transparent to the caller).
6. `OnToolCalled` fires as a diagnostic observation of the whole round-trip.

**New file:** `Private/SmokeTests/InoAgentsLiteRtLmConversationToolTest.cpp` registering `InoAgents.LiteRtLm.ConversationToolTest`. Test constructs and registers a `ULiteRtLmAddNumbersTool`, creates a conversation, sends `"What is 27 plus 15?"`, logs `OnToken` for visible progress, logs `OnToolCalled` for the tool round-trip, and logs `OnComplete` for the final answer.

**Success criteria:** `OnToolCalled` fires with `add_numbers` + `{a:27,b:15}` + `42`, and `OnComplete` fires with a final answer that includes "42" or "forty-two".

## 9. Test plan

| Sub-milestone | Console command | What it exercises |
|---|---|---|
| D.1 | `InoAgents.LiteRtLm.SubsystemLoadTest` | Subsystem construction, async model load off the game thread, single-cast dynamic delegate firing back on game thread, unload. |
| D.2 | `InoAgents.LiteRtLm.ConversationSendTest` | Conversation creation via subsystem factory, non-streaming blocking send on worker thread, `OnComplete` multicast, conversation destruction, worker thread join. |
| D.3 | `InoAgents.LiteRtLm.ConversationStreamTest` | Streaming generation, per-chunk `OnToken` marshaling, `Cancel` support, `OnComplete` at end. |
| D.4 | `InoAgents.LiteRtLm.ConversationToolTest` | Tool registration, tool-call detection during streaming, `Execute` invocation on game thread, result feedback loop, `OnToolCalled` diagnostic delegate, full agent loop. |

The pre-existing Phase 1 smoke tests (`InoAgents.LoadEngineTest`, `InoAgents.GenerateTest`, `InoAgents.ConversationTest`, `InoAgents.ToolCallTest`, `InoAgents.StreamTest`) **stay in the codebase unchanged**. They continue to exercise the raw native layer directly, independent of the new UE API, so regressions in either layer can be diagnosed without the other being a suspect.

## 10. Naming rationale (why "LiteRtLm" and why "Conversation")

### Why `LiteRtLm` prefix and not `InoAgents`

The plugin is `InoAgents`. Future versions will host multiple backends (OpenAI API, Anthropic API, llama.cpp, etc.), each implemented as its own subsystem + conversation + tool family under a subdirectory of `Source/InoAgents/Public/<Backend>/`. Naming the current classes `UInoAgentsSubsystem` would pretend there is only one backend and force renaming when the second one arrives. Naming them `ULiteRtLmSubsystem` makes the backend explicit from day one.

The full form `UInoAgentsLiteRtLmSubsystem` was considered and rejected as unnecessarily verbose. The plugin namespace is implicit in `Source/InoAgents/`; the `LiteRtLm` prefix alone is distinctive enough that practical class-name collision with unrelated plugins is vanishingly unlikely.

### Why `Conversation` and not `Agent` / `Session`

- **`Session`:** misleading. `LiteRtLmSession` is already a native C type in `c/engine.h` (raw generation session). Our UObject wraps `LiteRtLmConversation*`, not `LiteRtLmSession*`. Calling it `ULiteRtLmSession` would imply the wrong internal mapping.
- **`Agent`:** ambiguous. The plugin is named `InoAgents` for the framework-level concept. Calling one specific UObject inside the plugin "the agent" creates confusion about what "agent" refers to at what level.
- **`Conversation`:** accurate and honest. The UObject is a 1:1 UE wrapper around the native `LiteRtLmConversation*`. What Blueprint users create, use, and destroy **is** a conversation with the LLM. The `U` prefix disambiguates from the native type per standard UE convention.

### Why no shared base interface

Extracting a shared interface across backends requires knowing what the backends have in common. We currently have exactly one backend (LiteRT-LM). Any interface we extract now would be speculation about what OpenAI's or Anthropic's backends will look like, and the first time reality disagrees with the speculation we'd have to rework everything. Better to wait until a second backend exists, then extract common shape from real evidence.

## 11. What this milestone explicitly preserves unchanged

- The phase-1 smoke tests in `Source/InoAgents/Private/SmokeTests/*.cpp` (5 files) stay exactly as they are. They test the native layer directly and serve as regression baselines.
- `Source/InoAgents/Private/InoAgents.cpp` (module lifecycle + DLL loading) stays at 139 lines.
- `Source/InoAgents/Private/InoAgentsLog.h` stays as the shared log category for the whole module.
- `Source/ThirdParty/InoAgentsLibrary/` (the External module consuming the built DLL) is untouched.
- `Plugins/InoAgents/LiteRtLm/` (the Bazel build workspace) is untouched.

Milestone D is purely additive inside `Source/InoAgents/Public/LiteRtLm/` and `Source/InoAgents/Private/LiteRtLm/`, with the four new smoke test files added to `Source/InoAgents/Private/SmokeTests/`.

## 12. Open questions reserved for implementation

These are not design decisions but implementation details that are fine to settle at coding time:

- Exact `FRunnable` thread priority (`TPri_Normal` is fine as a default; revisit if generation hitches the game thread on slower machines).
- Whether `ULiteRtLmConversation::SendMessageAsync` uses a single-producer-single-consumer queue or a simpler `FCriticalSection`-guarded `TArray<FString>` for pending messages.
- Exact UTF-8 buffer lifetime management inside the worker — `FTCHARToUTF8` stack locals vs heap-allocated matching the Phase 1 `FInoAgentsStreamTestState` pattern.
- The format of the internal "tool call detected" signal between the C callback and the worker thread — depends on whether LiteRT-LM delivers tool calls as JSON chunks in the regular stream or via a separate callback mechanism.

These are left deliberately unresolved so the implementation plan can make concrete choices based on what works best when the code is actually being written.

---

## Approval history

- **2026-04-11:** initial design drafted collaboratively during brainstorming session. Four approval questions raised (naming, ownership, engine exposure, sub-milestone order); user approved all four.
- **2026-04-11:** design revised for backend-specific naming per user feedback. `UInoAgentsSubsystem` → `ULiteRtLmSubsystem`, files moved to `LiteRtLm/` subdirectory, smoke tests renamed with `.LiteRtLm.` infix.
- **2026-04-11:** class name for the conversation UObject settled as `ULiteRtLmConversation` (rejected `Agent` as redundant-with-plugin-name and `Session` as misleading vs the native type).
- **2026-04-11:** design document written, awaiting user review before proceeding to implementation plan.
