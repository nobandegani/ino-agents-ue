# Milestone D — LiteRT-LM UE API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the UE-facing API (subsystem + conversation + tool interface + model config) that wraps the LiteRT-LM native integration proven in Phase 1, so Blueprint and C++ gameplay code can load models, create conversations, send messages, receive streaming responses, and invoke tools without touching the native C API directly.

**Architecture:** Four-class split. `UGameInstanceSubsystem` owns the engine + tool registry (one per game instance). `UObject` conversations own a `LiteRtLmConversation*` + a dedicated worker `FRunnable` (one per chat). `UInterface` tool contract allows Blueprint-implementable tools. `UDataAsset` model config allows designer-editable model selection. Worker-per-conversation threading with `AsyncTask(ENamedThreads::GameThread, ...)` marshaling for every Blueprint-visible callback — the same pattern proven in Phase 1 milestone C.

**Tech Stack:** UE 5.7, C++, UObject reflection system, `IConsoleManager` for smoke tests, `IPluginManager`, UE `Json` module, LiteRT-LM v0.10.1 C API (native, via delay-loaded `LiteRtLm.dll`).

**Spec:** `Plugins/InoAgents/docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md`

---

## Pre-flight checklist

Before starting Task 1.1:

- [ ] **Git state is clean.** `cd Plugins/InoAgents && git status --short` must be empty or contain only changes you're OK committing separately first.
- [ ] **Phase 1 smoke tests pass as a regression baseline.** Rebuild the plugin and run each of these from the editor's Output Log command input. Each must produce its expected output (see the respective file's top-of-file comment block for the expected behavior):
  - `InoAgents.LoadEngineTest`
  - `InoAgents.GenerateTest`
  - `InoAgents.ConversationTest`
  - `InoAgents.ToolCallTest`
  - `InoAgents.StreamTest`
- [ ] **The Gemma 4 E2B model file is present** at `Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm` (2.41 GB).
- [ ] **Bazel build outputs are staged.** `ls Plugins/InoAgents/Source/ThirdParty/InoAgentsLibrary/Win64/LiteRtLm.lib` returns the import lib, and `ls Plugins/InoAgents/Binaries/ThirdParty/InoAgentsLibrary/Win64/{LiteRtLm.dll,libGemmaModelConstraintProvider.dll}` returns both runtime DLLs. If any are missing, run `Plugins/InoAgents/LiteRtLm/scripts/build-win64.ps1` first.
- [ ] **The design spec exists at** `Plugins/InoAgents/docs/superpowers/specs/2026-04-11-milestone-d-litert-lm-ue-api-design.md`. This plan references it.

All good? Start at Task 1.1.

---

## Shared conventions across all tasks

- **Paths** in this plan are relative to the repository root (`E:/Projects/InoAgentDemo/`) unless otherwise noted.
- **Commits** happen ONLY at the end of each sub-milestone, NOT per-task. This is explicitly per user preference: "commit at every test boundary, before test". Sub-milestone D.1 is one commit, D.2 is another, etc. Four commits total for Milestone D.
- **Rebuild + smoke test** at the end of each sub-milestone is a USER action, not something the agent can do directly. The agent stops, asks the user to rebuild and run, and waits for the result before committing and moving on.
- **License header** on every new file:
  ```cpp
  // Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.
  ```
- **Log category** in every file that logs: `#include "InoAgentsLog.h"` and use `UE_LOG(LogInoAgents, ...)`.
- **UE include style:** `#include "CoreMinimal.h"` in headers, specific subsystem headers in .cpp. Avoid `#include "Engine.h"`.
- **No `#pragma once` + `#include` guards both.** Just `#pragma once`.

---

## Sub-milestone D.1: `ULiteRtLmSubsystem` skeleton + `ULiteRtLmModelConfig` + async model load

**Goal:** Asynchronously load a LiteRT-LM engine from a `ULiteRtLmModelConfig` data asset, without freezing the editor, reporting success or failure via a game-thread delegate. At the end of D.1, the subsystem can load, report status, and unload — but cannot yet create conversations or register tools.

**Testable via:** `InoAgents.LiteRtLm.SubsystemLoadTest` console command.

**Files created in D.1 (in order):**

1. `Source/InoAgents/Public/LiteRtLm/LiteRtLmTypes.h` — enum + ALL 5 delegate declarations (centralized upfront; later sub-milestones reference but don't modify)
2. `Source/InoAgents/Private/LiteRtLm/LiteRtLmTypes.cpp` — `LiteRtLmBackendToString` helper
3. `Source/InoAgents/Public/LiteRtLm/LiteRtLmModelConfig.h`
4. `Source/InoAgents/Private/LiteRtLm/LiteRtLmModelConfig.cpp` (empty body)
5. `Source/InoAgents/Public/LiteRtLm/LiteRtLmSubsystem.h` — D.1 public API only
6. `Source/InoAgents/Private/LiteRtLm/LiteRtLmSubsystem.cpp` — D.1 implementation
7. `Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.h` — test observer `UCLASS`
8. `Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.cpp` — console command

**Files modified in D.1:**

- `Source/InoAgents/InoAgents.Build.cs` — add `Private/LiteRtLm` to `PrivateIncludePaths`

### Task 1.1: Create `LiteRtLmTypes.h`

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Public/LiteRtLm/LiteRtLmTypes.h`

- [ ] **Step 1: Create the file with the full content below.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "LiteRtLmTypes.generated.h"

/**
 * Which inference backend the LiteRT-LM engine uses. Maps to the
 * `backend_str` argument of litert_lm_engine_settings_create().
 */
UENUM(BlueprintType)
enum class ELiteRtLmBackend : uint8
{
    Cpu  UMETA(DisplayName="CPU"),
    Gpu  UMETA(DisplayName="GPU (D3D12 via WebGPU accelerator on Windows)"),
};

/**
 * Convert ELiteRtLmBackend to the C string LiteRT-LM expects. The returned
 * pointer is a static string literal — do not free it, do not copy it, its
 * lifetime is the module's lifetime.
 */
INOAGENTS_API const char* LiteRtLmBackendToString(ELiteRtLmBackend Backend);

// ============================================================================
// Delegates
// ============================================================================
//
// All five delegates the Milestone D API exposes are declared here upfront,
// even though only FOnLiteRtLmModelLoaded is used by the D.1 sub-milestone.
// Centralizing them avoids header churn across sub-milestones and makes the
// full Blueprint API surface discoverable in one place.
//
// Dynamic delegates are Blueprint-visible but require binding via UFUNCTION-
// flagged methods on UObjects. Non-dynamic delegates support BindLambda but
// are not Blueprint-visible. We use dynamic delegates throughout because the
// API's primary audience is Blueprint designers.
// ============================================================================

/**
 * Fired once by UInoAgentsSubsystem::LoadModelAsync when the load completes
 * (successfully or otherwise). Single-cast: one LoadModelAsync call attaches
 * one handler; there is no multicast model-loaded event.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnLiteRtLmModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * Fired per streaming chunk from ULiteRtLmConversation::SendMessageAsync.
 * Chunks are delivered on the game thread via AsyncTask. Blueprint code
 * typically binds a UMG text widget to this and appends chunks in real time.
 * Multicast so multiple observers (UI + logger + metrics panel, etc.) can
 * all watch.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmToken,
    FString, Chunk);

/**
 * Fired exactly once per successful SendMessageAsync call, after all tokens
 * have been delivered and any tool calls have been handled. FullText is the
 * concatenation of every Chunk broadcast during this send, with no trimming.
 * Either OnComplete or OnError fires per send, never both.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmComplete,
    FString, FullText);

/**
 * Fired exactly once per failed SendMessageAsync call. ErrorMessage describes
 * the failure in human-readable terms. Mutually exclusive with OnComplete.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmError,
    FString, ErrorMessage);

/**
 * Diagnostic event fired after ULiteRtLmConversation has handled a tool call
 * end-to-end (looked up the tool in the subsystem's registry, invoked it on
 * the game thread, fed the result back into the LiteRT-LM conversation).
 *
 * Most Blueprint graphs do NOT need to bind this — the conversation handles
 * tool calls transparently. The delegate exists so debug UI can observe the
 * full tool-call round-trip.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnLiteRtLmToolCalled,
    FName, ToolName,
    FString, ArgumentsJson,
    FString, ResultJson);
```

- [ ] **Step 2: Verify the file exists and has the expected size.**

Run:
```bash
wc -l Plugins/InoAgents/Source/InoAgents/Public/LiteRtLm/LiteRtLmTypes.h
```

Expected: approximately 90 lines. If it's much shorter, something got truncated.

### Task 1.2: Create `LiteRtLmTypes.cpp`

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Private/LiteRtLm/LiteRtLmTypes.cpp`

- [ ] **Step 1: Create the file.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmTypes.h"

const char* LiteRtLmBackendToString(ELiteRtLmBackend Backend)
{
    switch (Backend)
    {
        case ELiteRtLmBackend::Cpu: return "cpu";
        case ELiteRtLmBackend::Gpu: return "gpu";
    }
    // Defensive fallback: if the enum gains a new value in the future and
    // this switch isn't updated, we default to CPU rather than returning
    // a dangling pointer. Also log so the omission is visible.
    return "cpu";
}
```

- [ ] **Step 2: Verify the file compiles later** (it won't compile until Task 1.3+ give it something to include with, but it is self-consistent now). No action needed this step.

### Task 1.3: Create `LiteRtLmModelConfig.h` and `.cpp`

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Public/LiteRtLm/LiteRtLmModelConfig.h`
- Create: `Plugins/InoAgents/Source/InoAgents/Private/LiteRtLm/LiteRtLmModelConfig.cpp`

- [ ] **Step 1: Create `LiteRtLmModelConfig.h`.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmModelConfig.generated.h"

/**
 * Designer-editable configuration for a LiteRT-LM model.
 *
 * Create one of these as a Content Browser asset (right-click → Miscellaneous
 * → Data Asset → ULiteRtLmModelConfig) per model you want to ship. Point
 * LoadModelAsync at it to load the model.
 *
 * IMPORTANT: tool calling via constrained decoding is currently Gemma-family-
 * only because libGemmaModelConstraintProvider.dll is shipped only for Gemma
 * models. Pointing this config at a non-Gemma model (Qwen, generic, etc.)
 * will work for chat but may produce unreliable tool calling.
 */
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
     * This keeps configs portable across developer machines.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM")
    FString ModelFileName = TEXT("gemma-4-E2B-it.litertlm");

    /**
     * Which backend the engine should use.
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
     * Optional system message applied to conversations created from this
     * config. Plain text. The subsystem wraps it in the expected
     * {"type":"text","text":"..."} JSON shape before handing it to
     * LiteRT-LM — do not include JSON braces here.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM",
              meta=(MultiLine=true))
    FString SystemMessage;
};
```

- [ ] **Step 2: Create `LiteRtLmModelConfig.cpp`.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmModelConfig.h"

// This translation unit exists purely so UBT has a .cpp file to associate
// with the UDataAsset generated body. ULiteRtLmModelConfig has no custom
// logic — it is a pure data asset. If you find yourself adding methods to
// this .cpp, double-check that they genuinely belong on the model config
// and not on the subsystem (which already has a reference to the config
// after LoadModelAsync).
```

### Task 1.4: Create `LiteRtLmSubsystem.h` (D.1 API only)

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Public/LiteRtLm/LiteRtLmSubsystem.h`

**Note:** this file gets EXTENDED in D.2 (add `CreateConversation`) and D.4 (add tool registry methods). This task creates the D.1 version only. Later tasks will describe the additions surgically.

- [ ] **Step 1: Create the file.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmSubsystem.generated.h"

class ULiteRtLmModelConfig;

// Forward declarations of opaque native types from LiteRT-LM's C API.
// We deliberately do NOT include "litert/lm/engine.h" here — that would pull
// the C API surface into every translation unit that uses the subsystem.
// Instead, the subsystem .cpp includes it, and these forward declarations
// let the private pointer members type-check without exposing them to
// callers.
extern "C" {
    struct LiteRtLmEngine;
    struct LiteRtLmEngineSettings;
}

/**
 * Game-instance-wide LiteRT-LM runtime owner.
 *
 * ONE instance per game instance (created on game start, destroyed on game
 * shutdown). Accessed via:
 *
 *     UGameInstance* GI = GetGameInstance();
 *     ULiteRtLmSubsystem* Subsys = GI->GetSubsystem<ULiteRtLmSubsystem>();
 *
 * Owns:
 *   - The loaded LiteRtLmEngine* (expensive, shared across conversations)
 *   - [D.4] A map of registered ILiteRtLmTool implementations
 *
 * Does NOT own:
 *   - ULiteRtLmConversation instances — those are owned by their callers.
 *     The subsystem is a factory, not a registry.
 *
 * Lifecycle:
 *   Initialize()   : called by UE at game start; zero-inits members.
 *                    Does NOT load a model — that would freeze the editor.
 *   LoadModelAsync(): called by game code to load a model. Async. Fires the
 *                    OnLoaded delegate on the game thread when done.
 *   UnloadModel()  : called to destroy the engine. Safe to call with no
 *                    model loaded. Does NOT automatically tear down active
 *                    conversations — the caller is responsible for that.
 *   Deinitialize() : called by UE at game shutdown; calls UnloadModel and
 *                    [D.4] clears the tool registry.
 */
UCLASS()
class INOAGENTS_API ULiteRtLmSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    //~ UGameInstanceSubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem interface

    // ------------------------------------------------------------------
    // Model lifecycle (D.1)
    // ------------------------------------------------------------------

    /**
     * Asynchronously load a LiteRT-LM engine from the given model config.
     * Returns immediately. When loading finishes (success or failure),
     * OnLoaded fires on the game thread.
     *
     * Error cases that fire OnLoaded with bSuccess=false:
     *   - Another load is already in flight
     *   - A model is already loaded (call UnloadModel first)
     *   - Config is null
     *   - The plugin cannot be located via IPluginManager
     *   - The model file does not exist at the resolved path
     *   - litert_lm_engine_settings_create returned NULL
     *   - litert_lm_engine_create returned NULL (most expensive failure;
     *     can happen for corrupt or unsupported models)
     *
     * MUST be called on the game thread. The actual engine construction
     * runs on a ThreadPool worker; the OnLoaded callback marshals back.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM",
              meta=(AutoCreateRefTerm="OnLoaded"))
    void LoadModelAsync(
        const ULiteRtLmModelConfig* Config,
        const FOnLiteRtLmModelLoaded& OnLoaded);

    /**
     * True if LoadModelAsync has successfully completed and UnloadModel has
     * not yet been called. False during an in-flight load.
     */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsModelLoaded() const;

    /**
     * Destroy the loaded engine. Safe to call with no model loaded (no-op).
     *
     * IMPORTANT: if any ULiteRtLmConversation instances are still alive,
     * their worker threads are still holding native LiteRtLmConversation
     * pointers that reference the engine. Calling UnloadModel while those
     * are active results in undefined behavior. The caller is responsible
     * for destroying all conversations before unloading. The subsystem
     * does NOT track outstanding conversations.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void UnloadModel();

private:
    // Opaque native handles. Never exposed to Blueprint. The UBT-visible
    // forward declarations at the top of this file make these type-check
    // without including the LiteRT-LM C header.
    LiteRtLmEngine*          Engine   = nullptr;
    LiteRtLmEngineSettings*  Settings = nullptr;

    // Kept alive while the model is loaded so that GC does not reclaim
    // the asset out from under us.
    UPROPERTY()
    TObjectPtr<const ULiteRtLmModelConfig> LoadedConfig;

    // True from the moment LoadModelAsync dispatches to the ThreadPool
    // until the OnLoaded callback fires back on the game thread.
    bool bLoadInFlight = false;
};
```

### Task 1.5: Create `LiteRtLmSubsystem.cpp` (D.1 implementation)

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Private/LiteRtLm/LiteRtLmSubsystem.cpp`

This is the most complex file in D.1 because of the async load pattern. Read the whole file before transcribing, then write it in one pass.

- [ ] **Step 1: Create the file.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmTypes.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// The native C API. Only included in this .cpp — callers of the subsystem
// never see LiteRT-LM types directly.
#include "litert/lm/engine.h"

void ULiteRtLmSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init members explicitly in case a fresh subsystem is ever
    // reused (hot reload in editor). Nothing expensive happens here —
    // engine loading is lazy via LoadModelAsync.
    Engine       = nullptr;
    Settings     = nullptr;
    LoadedConfig = nullptr;
    bLoadInFlight = false;

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: Initialize"));
}

void ULiteRtLmSubsystem::Deinitialize()
{
    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: Deinitialize"));

    // Note: if a load is in flight at shutdown, the ThreadPool worker is
    // still running. We do NOT block waiting for it — instead, the worker's
    // completion lambda captures a TWeakObjectPtr<ULiteRtLmSubsystem> and
    // no-ops if the subsystem is gone. This means a one-time leak of the
    // engine if shutdown races an in-flight load, which is acceptable
    // because the process is dying anyway.

    UnloadModel();
    Super::Deinitialize();
}

void ULiteRtLmSubsystem::LoadModelAsync(
    const ULiteRtLmModelConfig* Config,
    const FOnLiteRtLmModelLoaded& OnLoaded)
{
    check(IsInGameThread());

    if (bLoadInFlight)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LoadModelAsync: another load is already in flight; rejecting"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model load is already in flight"));
        return;
    }

    if (Engine != nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LoadModelAsync: a model is already loaded; call UnloadModel first"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model is already loaded; call UnloadModel first"));
        return;
    }

    if (Config == nullptr)
    {
        OnLoaded.ExecuteIfBound(false, TEXT("Config is null"));
        return;
    }

    // Resolve the model file path via IPluginManager. Done on the game
    // thread because IPluginManager access patterns assume game thread.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        OnLoaded.ExecuteIfBound(false, TEXT("IPluginManager could not locate the InoAgents plugin"));
        return;
    }

    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(BaseDir, TEXT("Models"), Config->ModelFileName);

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        const FString Err = FString::Printf(
            TEXT("Model file not found at %s. Expected to find %s under Plugins/InoAgents/Models/."),
            *ModelPath, *Config->ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // From here on we're committed to an async load. Transition state.
    bLoadInFlight = true;
    LoadedConfig  = Config;

    // Capture-by-value what the worker needs. Do NOT capture `this` or
    // `Config` directly — a TWeakObjectPtr lets us safely no-op if the
    // subsystem or config is gone by the time the load completes.
    TWeakObjectPtr<ULiteRtLmSubsystem> WeakThis(this);
    const FString                     ModelPathCopy = ModelPath;
    const ELiteRtLmBackend            BackendCopy   = Config->Backend;
    const double                      TStart        = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: dispatching async load of %s (backend=%s)"),
           *ModelPathCopy, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)));

    Async(EAsyncExecution::ThreadPool,
        [ModelPathCopy, BackendCopy, WeakThis, OnLoaded, TStart]()
    {
        // ============== WORKER THREAD ==============
        //
        // Safety rules:
        //   - Do NOT touch WeakThis here (except to pass it forward to the
        //     game-thread lambda). Weak pointer access is only valid from
        //     the game thread.
        //   - Do NOT UE_LOG from here. LogInoAgents IS thread-safe in
        //     principle, but we keep the worker silent so all logs come
        //     through the game-thread lambda below (single-threaded log
        //     order preserves readability).

        const FTCHARToUTF8 ModelPathUtf8(*ModelPathCopy);
        const char* const  BackendStr = LiteRtLmBackendToString(BackendCopy);

        LiteRtLmEngineSettings* NewSettings = litert_lm_engine_settings_create(
            ModelPathUtf8.Get(), BackendStr,
            /*vision_backend_str=*/ nullptr,
            /*audio_backend_str=*/ nullptr);

        FString          LocalError;
        LiteRtLmEngine*  NewEngine = nullptr;

        if (NewSettings == nullptr)
        {
            LocalError = TEXT("litert_lm_engine_settings_create returned NULL");
        }
        else
        {
            NewEngine = litert_lm_engine_create(NewSettings);
            if (NewEngine == nullptr)
            {
                LocalError = TEXT("litert_lm_engine_create returned NULL (check LiteRT-LM internal logs above)");
                litert_lm_engine_settings_delete(NewSettings);
                NewSettings = nullptr;
            }
        }

        const double Elapsed = FPlatformTime::Seconds() - TStart;

        // ============== HOP BACK TO GAME THREAD ==============
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, NewEngine, NewSettings, LocalError, Elapsed, OnLoaded]()
        {
            // Subsystem gone (game instance shutting down, or race with
            // Deinitialize). Clean up native resources and drop the result.
            if (!WeakThis.IsValid())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LoadModelAsync completion: subsystem is gone; freeing engine and giving up"));
                if (NewEngine)   litert_lm_engine_delete(NewEngine);
                if (NewSettings) litert_lm_engine_settings_delete(NewSettings);
                return;
            }

            ULiteRtLmSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;

            if (NewEngine == nullptr)
            {
                // Load failed. Clear the config reference so a subsequent
                // retry can succeed.
                Subsys->LoadedConfig = nullptr;
                UE_LOG(LogInoAgents, Error,
                       TEXT("LoadModelAsync: FAILED after %.2f s: %s"),
                       Elapsed, *LocalError);
                OnLoaded.ExecuteIfBound(false, LocalError);
                return;
            }

            // Success.
            Subsys->Engine   = NewEngine;
            Subsys->Settings = NewSettings;
            UE_LOG(LogInoAgents, Log,
                   TEXT("LoadModelAsync: SUCCESS in %.2f s"), Elapsed);
            OnLoaded.ExecuteIfBound(true, FString());
        });
    });
}

bool ULiteRtLmSubsystem::IsModelLoaded() const
{
    return Engine != nullptr;
}

void ULiteRtLmSubsystem::UnloadModel()
{
    check(IsInGameThread());

    if (Engine != nullptr)
    {
        litert_lm_engine_delete(Engine);
        Engine = nullptr;
    }
    if (Settings != nullptr)
    {
        litert_lm_engine_settings_delete(Settings);
        Settings = nullptr;
    }
    LoadedConfig = nullptr;

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: UnloadModel complete"));
}
```

### Task 1.6: Update `InoAgents.Build.cs`

**Files:**
- Modify: `Plugins/InoAgents/Source/InoAgents/InoAgents.Build.cs`

- [ ] **Step 1: Open the file and add the new entry to `PrivateIncludePaths`.**

The existing `PrivateIncludePaths.AddRange` call already has `Private/SmokeTests` from the refactor. Add `Private/LiteRtLm` alongside it:

Replace:
```csharp
		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the phase-1 smoke test
				// source files and their shared helpers. Adding it here lets
				// the test .cpp files #include "InoAgentsSmokeTestCommon.h"
				// and #include "InoAgentsLog.h" (the latter resolves via
				// Private/ which UBT already adds automatically).
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
			}
			);
```

With:
```csharp
		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the phase-1 smoke test
				// source files and their shared helpers. Adding it here lets
				// the test .cpp files #include "InoAgentsSmokeTestCommon.h"
				// and #include "InoAgentsLog.h" (the latter resolves via
				// Private/ which UBT already adds automatically).
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),

				// Subdirectory of Private/ that holds the LiteRT-LM backend
				// implementation (workers, tool impls, etc.). Added so that
				// LiteRtLmSubsystem.cpp can #include "LiteRtLmConversationWorker.h"
				// and similar private headers without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LiteRtLm"),
			}
			);
```

### Task 1.7: Create `InoAgentsLiteRtLmSubsystemLoadTest.h` (observer UCLASS)

**Why a .h file for a smoke test:** `FOnLiteRtLmModelLoaded` is a dynamic delegate and can only be bound via `BindDynamic(Object, &UClass::Method)` where `Method` is a `UFUNCTION`. `UFUNCTION` requires a `UCLASS` declared in a header file so that UnrealHeaderTool can generate reflection metadata. We put the observer in its own header, alongside the .cpp that uses it.

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.h`

- [ ] **Step 1: Create the file.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "InoAgentsLiteRtLmSubsystemLoadTest.generated.h"

class ULiteRtLmSubsystem;
class ULiteRtLmModelConfig;

/**
 * One-shot observer for the InoAgents.LiteRtLm.SubsystemLoadTest console
 * command. Holds a UFUNCTION callback that BindDynamic can target, plus a
 * UPROPERTY-kept-alive reference to the subsystem and model config so they
 * are not garbage-collected mid-load.
 *
 * Added to the GC root when the test starts, removed from root inside
 * HandleLoaded so the observer itself (and its held references) become
 * eligible for collection once the load completes.
 */
UCLASS()
class UInoAgentsLiteRtLmSubsystemLoadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;

    UPROPERTY()
    TObjectPtr<ULiteRtLmSubsystem> Subsystem = nullptr;

    UPROPERTY()
    TObjectPtr<ULiteRtLmModelConfig> Config = nullptr;

    UFUNCTION()
    void HandleLoaded(bool bSuccess, const FString& ErrorMessage);
};
```

### Task 1.8: Create `InoAgentsLiteRtLmSubsystemLoadTest.cpp`

**Files:**
- Create: `Plugins/InoAgents/Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.cpp`

- [ ] **Step 1: Create the file.**

```cpp
// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.LiteRtLm.SubsystemLoadTest (milestone D.1)
// ============================================================================
//
// Exercises the ULiteRtLmSubsystem's async model load path end-to-end:
//
//   1. Grab the subsystem from the current game instance.
//   2. Construct a ULiteRtLmModelConfig in C++ (not from a Content Browser
//      asset) pointing at the default Gemma 4 E2B model.
//   3. Call LoadModelAsync with a dynamic-delegate callback that logs
//      success/failure + wall-clock duration.
//   4. Verify IsModelLoaded() returns true in the success callback.
//   5. Call UnloadModel() immediately after verification.
//
// Runs non-blocking: the console command returns immediately and the
// editor stays responsive while the engine loads on a thread-pool worker.
// Log lines appear asynchronously.
//
// Invoke:
//     InoAgents.LiteRtLm.SubsystemLoadTest
// ============================================================================

#include "InoAgentsLiteRtLmSubsystemLoadTest.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace
{
    /**
     * Get a ULiteRtLmSubsystem from any game instance currently alive.
     * Console commands run outside of a specific world context, so we
     * iterate world contexts to find one.
     */
    ULiteRtLmSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (ULiteRtLmSubsystem* Subsys = GI->GetSubsystem<ULiteRtLmSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }
}

void UInoAgentsLiteRtLmSubsystemLoadTestObserver::HandleLoaded(
    bool bSuccess, const FString& ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    if (bSuccess)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemLoadTest: SUCCESS in %.2f s"), Elapsed);

        const bool bLoaded = Subsystem != nullptr && Subsystem->IsModelLoaded();
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemLoadTest: IsModelLoaded() returned %s after the delegate fired"),
               bLoaded ? TEXT("true") : TEXT("false"));

        if (Subsystem != nullptr)
        {
            Subsystem->UnloadModel();
            UE_LOG(LogInoAgents, Log,
                   TEXT("SubsystemLoadTest: unloaded successfully; IsModelLoaded() now returns %s"),
                   Subsystem->IsModelLoaded() ? TEXT("true") : TEXT("false"));
        }

        UE_LOG(LogInoAgents, Log, TEXT("SubsystemLoadTest: DONE"));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemLoadTest: FAILED after %.2f s: %s"),
               Elapsed, *ErrorMessage);
    }

    // Release our GC root anchor. After this, we (and our UPROPERTY-held
    // Subsystem + Config references) become eligible for collection.
    RemoveFromRoot();
}

static void RunLiteRtLmSubsystemLoadTest(const TArray<FString>& /*Args*/)
{
    ULiteRtLmSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemLoadTest: could not find a ULiteRtLmSubsystem. "
                    "This usually means there is no active game instance — "
                    "try running the test after entering PIE, or check that "
                    "the plugin module is loaded."));
        return;
    }

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("SubsystemLoadTest: a model is already loaded. Unloading first "
                    "so the test can run from a clean state."));
        Subsys->UnloadModel();
    }

    // Build the config inline. Using NewObject instead of a Content Browser
    // asset makes the test entirely self-contained — there's no asset to
    // create before running.
    ULiteRtLmModelConfig* Config = NewObject<ULiteRtLmModelConfig>();
    Config->ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config->Backend       = ELiteRtLmBackend::Cpu;
    Config->SystemMessage = TEXT("You are a helpful assistant.");

    // Observer holds the async continuation. Must outlive the delegate
    // callback, so root it.
    auto* Observer = NewObject<UInoAgentsLiteRtLmSubsystemLoadTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->Config    = Config;
    Observer->AddToRoot();

    FOnLiteRtLmModelLoaded Delegate;
    Delegate.BindDynamic(Observer, &UInoAgentsLiteRtLmSubsystemLoadTestObserver::HandleLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemLoadTest: starting — kicking off LoadModelAsync (non-blocking)"));

    Subsys->LoadModelAsync(Config, Delegate);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemLoadTest: LoadModelAsync returned synchronously. Editor stays responsive."));
}

static FAutoConsoleCommand GLiteRtLmSubsystemLoadTestCommand(
    TEXT("InoAgents.LiteRtLm.SubsystemLoadTest"),
    TEXT("Milestone D.1 smoke test: kicks off ULiteRtLmSubsystem::LoadModelAsync "
         "with an inline ULiteRtLmModelConfig, logs the result from the dynamic "
         "delegate callback on the game thread, unloads the model, exits. "
         "Non-blocking — the editor stays responsive during the ~0.5-2.5 s load."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmSubsystemLoadTest));
```

### Task 1.9: Rebuild, run the test, and commit

- [ ] **Step 1: Announce to the user that D.1 is code-complete and ready to test.**

Message the user: all D.1 files are written. Ask them to:
1. Rebuild the plugin (whatever IDE / build method they use)
2. Open the editor
3. Verify the normal startup log lines still appear (no regression in module lifecycle)
4. In the Output Log command input, run: `InoAgents.LiteRtLm.SubsystemLoadTest`
5. Paste the resulting log output

- [ ] **Step 2: Interpret the user's log output.**

Expected successful output (timings are machine-specific):
```
LogInoAgents: SubsystemLoadTest: starting — kicking off LoadModelAsync (non-blocking)
LogInoAgents: LoadModelAsync: dispatching async load of E:/Projects/InoAgentDemo/Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm (backend=cpu)
LogInoAgents: SubsystemLoadTest: LoadModelAsync returned synchronously. Editor stays responsive.
... (brief pause while the worker thread loads the engine)
LogInoAgents: LoadModelAsync: SUCCESS in 0.XX s
LogInoAgents: SubsystemLoadTest: SUCCESS in 0.XX s
LogInoAgents: SubsystemLoadTest: IsModelLoaded() returned true after the delegate fired
LogInoAgents: ULiteRtLmSubsystem: UnloadModel complete
LogInoAgents: SubsystemLoadTest: unloaded successfully; IsModelLoaded() now returns false
LogInoAgents: SubsystemLoadTest: DONE
```

Possible failure modes and diagnostic interpretations:
- **Build fails with "unresolved external symbol LiteRtLmBackendToString"** → `LiteRtLmTypes.cpp` didn't get compiled. Verify the file was created in Task 1.2 and that `Source/InoAgents/Private/LiteRtLm/` is NOT excluded by UBT.
- **Build fails with "undefined identifier FOnLiteRtLmToken" (or similar) in some other file** → a later-sub-milestone file was accidentally created during D.1. Only Task 1.1-1.8 files should exist after D.1.
- **Build succeeds, test runs, "could not find a ULiteRtLmSubsystem"** → there is no active game instance. If running in a blank editor without any map loaded, this can happen. Open any map in the editor first, then run the test. (Non-issue in shipping — a running game always has a game instance.)
- **Test runs, "Model file not found at ..."** → the plugin's Models/ directory is missing the file, or the path has unexpected characters. Check disk, rerun Bazel build if needed.
- **Test runs, "engine_create returned NULL"** → LiteRT-LM could not load the model. Check the Output Log for any LiteRT-LM internal error messages appearing before the "FAILED" log line. Most likely causes: corrupted model file, insufficient RAM, unsupported backend.
- **Editor freezes during the test** → `LoadModelAsync` is running synchronously on the game thread instead of the worker. Check that the `Async(EAsyncExecution::ThreadPool, ...)` call in `LiteRtLmSubsystem.cpp` actually dispatches to a worker (UE 5.7 should handle this correctly out of the box).

- [ ] **Step 3: Commit D.1.**

Only after the user confirms the smoke test passes:

```bash
cd E:/Projects/InoAgentDemo/Plugins/InoAgents

git add \
  Source/InoAgents/InoAgents.Build.cs \
  Source/InoAgents/Public/LiteRtLm/LiteRtLmTypes.h \
  Source/InoAgents/Public/LiteRtLm/LiteRtLmModelConfig.h \
  Source/InoAgents/Public/LiteRtLm/LiteRtLmSubsystem.h \
  Source/InoAgents/Private/LiteRtLm/LiteRtLmTypes.cpp \
  Source/InoAgents/Private/LiteRtLm/LiteRtLmModelConfig.cpp \
  Source/InoAgents/Private/LiteRtLm/LiteRtLmSubsystem.cpp \
  Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.h \
  Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.cpp

git commit -m "$(cat <<'EOF'
Milestone D.1: ULiteRtLmSubsystem + async model load

Introduces the UE-facing subsystem that wraps the LiteRT-LM engine,
plus the ULiteRtLmModelConfig UDataAsset that designers use to pick
which .litertlm file to load.

New public surface (in Source/InoAgents/Public/LiteRtLm/):
  - LiteRtLmTypes.h: ELiteRtLmBackend enum, all 5 dynamic delegate
    types declared upfront (only FOnLiteRtLmModelLoaded used in D.1;
    D.2-D.4 use the remaining four).
  - LiteRtLmModelConfig.h: UDataAsset with ModelFileName, Backend,
    MaxNumTokens, SystemMessage. Designer-editable.
  - LiteRtLmSubsystem.h: UGameInstanceSubsystem with Initialize,
    Deinitialize, LoadModelAsync, IsModelLoaded, UnloadModel.
    CreateConversation + tool registry added in later sub-milestones.

New private impl (in Source/InoAgents/Private/LiteRtLm/):
  - LiteRtLmTypes.cpp: LiteRtLmBackendToString switch.
  - LiteRtLmSubsystem.cpp: LoadModelAsync dispatches to the UE
    thread pool via Async(EAsyncExecution::ThreadPool, ...) and
    marshals the result back to the game thread via AsyncTask
    with a TWeakObjectPtr guard for subsystem-gone-by-the-time-
    the-callback-fires.

New D.1 smoke test:
  - SmokeTests/InoAgentsLiteRtLmSubsystemLoadTest.{h,cpp} registering
    the InoAgents.LiteRtLm.SubsystemLoadTest console command. The
    .h holds a tiny UObject observer whose UFUNCTION HandleLoaded
    method is bound to the dynamic delegate (dynamic delegates
    cannot be BindLambda'd).

Build.cs change: InoAgents.Build.cs PrivateIncludePaths gains
Private/LiteRtLm/ so .cpp files in that subdirectory can include
sibling private headers without relative paths.

No changes to pre-existing phase-1 smoke tests or module lifecycle
code. Verified by user running the D.1 smoke test from the editor
console and seeing the expected non-blocking load + unload output.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"

git status --short
```

Expected: `git status --short` returns empty after the commit.

D.1 is complete. Move on to D.2.

---

## Sub-milestone D.2: `ULiteRtLmConversation` + non-streaming `SendMessageAsync`

**Goal:** Introduce the UObject-based conversation wrapper, a dedicated worker `FRunnable` per conversation, and the non-streaming send path. After D.2, game code can create a conversation from the subsystem, send a user message, and receive the assistant's full response on the game thread via a delegate — still without editor freezes. Streaming (D.3) and tool calling (D.4) layer on top of this foundation.

**Testable via:** `InoAgents.LiteRtLm.ConversationSendTest` console command.

**Non-negotiable threading rules** (from spec section 6):
- Worker-per-conversation isolation (one pinned `FRunnableThread` per `ULiteRtLmConversation`).
- Worker → game thread marshaling exclusively via `AsyncTask(ENamedThreads::GameThread, ...)` with a captured `TWeakObjectPtr<ULiteRtLmConversation>`.
- The worker never touches `UObject` pointers directly. The game-thread lambda checks the weak pointer for validity before dereferencing.
- Conversation destruction (via `BeginDestroy`) joins the worker thread *before* destroying native resources.

### Files created in D.2

1. `Source/InoAgents/Public/LiteRtLm/LiteRtLmConversation.h` — `UCLASS(BlueprintType) ULiteRtLmConversation : public UObject` with `SendMessageAsync`, `OnComplete`, `OnError`, `BeginDestroy`, and the internal `Initialize(Subsystem, Engine, Config)` that the factory calls.
2. `Source/InoAgents/Private/LiteRtLm/LiteRtLmConversationWorker.h` — private `class FLiteRtLmConversationWorker : public FRunnable`. Holds the native `LiteRtLmConversation*` + `LiteRtLmConversationConfig*`, a `TQueue<FString, Spsc>` for pending messages, an `FEvent` for wake-up, a `TAtomic<bool>` stop flag, and a `TUniquePtr<FRunnableThread>`.
3. `Source/InoAgents/Private/LiteRtLm/LiteRtLmConversationWorker.cpp` — constructor starts the thread, destructor signals stop and joins. `Run()` is a `TQueue::Dequeue` loop with `FEvent::Wait` when empty. `ProcessMessage` builds the user-message JSON (reusing the Phase 1 `EscapeJsonString` + format pattern), calls blocking `litert_lm_conversation_send_message`, parses the response via `FJsonObject` / `TJsonReaderFactory`, extracts concatenated `content[*].text`, and dispatches `OnComplete` via `AsyncTask`. Errors dispatch `OnError`.
4. `Source/InoAgents/Private/LiteRtLm/LiteRtLmConversation.cpp` — `Initialize` wraps the config's plain-text `SystemMessage` in JSON, calls `litert_lm_conversation_config_create` + `litert_lm_conversation_create`, hands the native pointers to a new `FLiteRtLmConversationWorker`. `SendMessageAsync` enqueues on the worker. `BeginDestroy` resets the worker's `TUniquePtr`, which runs the worker's destructor (join + native cleanup).
5. `Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmConversationSendTest.h` — observer `UCLASS` with `HandleModelLoaded`, `HandleConversationComplete`, `HandleConversationError` `UFUNCTION`s. All `FString` parameters are by value (per D.1 lesson).
6. `Source/InoAgents/Private/SmokeTests/InoAgentsLiteRtLmConversationSendTest.cpp` — registers `InoAgents.LiteRtLm.ConversationSendTest`. Orchestrates: load-if-not-loaded → create conversation → bind delegates → `SendMessageAsync` → log response → release observer. Reuses an already-loaded model across test runs (skips the load step if `IsModelLoaded()` returns true).

### Files modified in D.2

- `Source/InoAgents/Public/LiteRtLm/LiteRtLmSubsystem.h` — forward-declare `ULiteRtLmConversation`; add `UFUNCTION(BlueprintCallable) ULiteRtLmConversation* CreateConversation()`.
- `Source/InoAgents/Private/LiteRtLm/LiteRtLmSubsystem.cpp` — `#include "LiteRtLm/LiteRtLmConversation.h"`; implement `CreateConversation` as a factory that `NewObject<ULiteRtLmConversation>()` + `Initialize(this, Engine, LoadedConfig)`.

### Key design decisions (why D.2 looks the way it does)

- **Smoke test does NOT unload the model on exit.** Reason: if it did, the subsystem's `UnloadModel()` would destroy the engine while the conversation's worker might still be finishing its `BeginDestroy → Worker.Reset()` path (UE GC is deferred — setting a `TObjectPtr` to null does not destroy the UObject immediately). The worker's destructor tries to call `litert_lm_conversation_delete` on its native pointer, which references the engine — dangling. Leaving the model loaded sidesteps the race entirely. Subsequent test runs reuse the loaded model via an `IsModelLoaded()` check at the top of the test. Editor tear-down (PIE stop or editor close) handles final cleanup via `Subsystem::Deinitialize → UnloadModel`.
- **All dynamic delegate handlers take `FString` by value**, not `const FString&`. UE's `BindDynamic` does strict method-pointer type matching against the `DECLARE_DYNAMIC_*_Param` macro's type list. `const FString&` does not match `FString` even though both work for read-only semantics. This is documented on both the D.1 observer and the D.2 observer so the pattern is clear for D.3 and D.4.
- **Response text extraction inlined in `LiteRtLmConversationWorker.cpp`**, not factored into a shared helper with the Phase 1 smoke tests. The smoke tests' `InoAgentsSmokeTest::ExtractAssistantText` is dev-time code; the worker is production code. They happen to do the same thing today, but the production version will diverge in D.4 when it needs to distinguish `tool_calls` from plain text. Premature factoring would create coupling that hurts the D.4 refactor.
- **Empty assistant text is treated as an error**, not as `OnComplete("")`. An empty response usually indicates a template / tool-call edge case and is not what a plain-chat caller expects. If this turns out to be wrong in practice (some model legitimately returns an empty success response), we relax the check — but defaulting to "raise it early" catches silent failures.
- **`TAtomic<bool>` + `FEvent` for the worker's wake-up loop**, not `std::condition_variable`. UE-idiomatic pattern, plays nicely with `FRunnableThread`'s shutdown expectations.

### Task breakdown

- [x] **Task 2.1:** Write `LiteRtLmConversation.h` (public header). Done.
- [x] **Task 2.2:** Write `LiteRtLmConversationWorker.{h,cpp}` (private worker). Done.
- [x] **Task 2.3:** Write `LiteRtLmConversation.cpp` (implementation). Done.
- [x] **Task 2.4:** Extend `LiteRtLmSubsystem.{h,cpp}` with `CreateConversation`. Done.
- [x] **Task 2.5:** Write `InoAgentsLiteRtLmConversationSendTest.{h,cpp}` smoke test. Done.
- [x] **Task 2.6:** Append D.2 section to this plan doc. Done (this section).
- [ ] **Task 2.7:** User rebuilds, opens the editor, enters PIE, runs `InoAgents.LiteRtLm.ConversationSendTest`, confirms the expected output. On success, commit. On failure, diagnose from the log.

### Expected successful output for `InoAgents.LiteRtLm.ConversationSendTest`

```
(PIE start)
LogInoAgents: ULiteRtLmSubsystem: Initialize
(user types: InoAgents.LiteRtLm.ConversationSendTest)
LogInoAgents: ConversationSendTest: starting — loading model first (non-blocking)
LogInoAgents: LoadModelAsync: dispatching async load of .../gemma-4-E2B-it.litertlm (backend=cpu)
LogInoAgents: LoadModelAsync: SUCCESS in 0.2X s
LogInoAgents: ConversationSendTest: model loaded, creating conversation
LogInoAgents: ULiteRtLmConversation: initialized (system_message=<set>)
LogInoAgents: FLiteRtLmConversationWorker: thread started
LogInoAgents: ConversationSendTest: sending prompt: "What is 2 plus 2? Answer in one sentence."
(brief pause while the worker runs litert_lm_conversation_send_message)
LogInoAgents: ConversationSendTest: response received in X.XX s (total test elapsed)
LogInoAgents: ConversationSendTest: assistant text: "Two plus two equals four."
LogInoAgents: ConversationSendTest: DONE
(later, on PIE stop)
LogInoAgents: FLiteRtLmConversationWorker: destroyed
LogInoAgents: ULiteRtLmSubsystem: Deinitialize
LogInoAgents: ULiteRtLmSubsystem: UnloadModel complete
```

The key things I want to see:
- `FLiteRtLmConversationWorker: thread started` appears during conversation creation (confirms the worker thread actually started)
- `response received in X.XX s` (X is the full round-trip time including blocking `send_message`)
- Assistant text is coherent and answers the question (tests that the chat template is applied, which was Milestone A's Phase 1 test — D.2 reuses the same path)
- `FLiteRtLmConversationWorker: destroyed` appears at PIE stop (confirms the worker thread joined cleanly)
- No crashes, no warnings about dangling weak pointers, no `OnError` unexpectedly

---
