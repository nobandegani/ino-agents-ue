// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ScriptInterface.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"  // FInoLiteRtLmDownloadProgressDelegate
                                        // (transitively pulls FInoCancellationToken via InoDownloader.h)

#include "InoLiteRtLmSubsystem.generated.h"

class UInoLiteRtLmConversation;
class UInoLiteRtLmToolBase;

// Forward declarations of opaque native types from LiteRT-LM's C API.
// We deliberately do NOT include "litert/lm/engine.h" here — that would pull
// the C API surface into every translation unit that uses the subsystem.
// Instead, the subsystem .cpp includes it, and these forward declarations
// let the private pointer members type-check without exposing them to
// callers.
extern "C" {
    struct LiteRtLmEngine;
}

/**
 * Game-instance-wide LiteRT-LM runtime owner.
 *
 * ONE instance per game instance (created on game start, destroyed on game
 * shutdown). Accessed via:
 *
 *     UGameInstance* GI = GetGameInstance();
 *     UInoLiteRtLmSubsystem* Subsys = GI->GetSubsystem<UInoLiteRtLmSubsystem>();
 *
 * Owns:
 *   - The loaded LiteRtLmEngine* (expensive, shared across conversations)
 *   - A map of registered UInoLiteRtLmToolBase instances
 *   - A weak reference to the single currently-active UInoLiteRtLmConversation,
 *     used to enforce "one conversation per engine" and to tear it down
 *     before the engine at shutdown time.
 *
 * Single-conversation invariant:
 *   LiteRT-LM sessions on the same engine share a single LlmExecutor (and
 *   thus a single KV cache) — see runtime/core/engine_impl.cc:157. Upstream
 *   tests serialise session use with an explicit `session->reset()` before
 *   each new `CreateSession()` call, and the plugin enforces the same
 *   invariant: CreateConversation auto-shuts-down any prior active
 *   conversation before returning a new one.
 *
 * Lifecycle:
 *   Initialize()   : called by UE at game start; zero-inits members.
 *                    Does NOT load a model — that would freeze the editor.
 *   LoadModelAsync(): called by game code to load a model. Async. Fires the
 *                    OnLoaded delegate on the game thread when done.
 *   UnloadModel()  : called to destroy the engine. Safe to call with no
 *                    model loaded. Shuts down the active conversation (if
 *                    any) before destroying the engine, so native resources
 *                    are always torn down in a safe order.
 *   Deinitialize() : called by UE at game shutdown; calls UnloadModel and
 *                    Clears the tool registry.
 */
UCLASS()
class INOLITERTLM_API UInoLiteRtLmSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    //~ UGameInstanceSubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem interface

    // ------------------------------------------------------------------
    // Model lifecycle
    // ------------------------------------------------------------------

    /**
     * Asynchronously load a LiteRT-LM engine from the given model config.
     * Returns immediately; OnLoaded fires on the game thread when done.
     *
     * Flow:
     *   1. Resolve the registry entry from `UInoLiteRtLmSettings::Models`
     *      via `FindModel` (matches DisplayName OR LocalFileName).
     *   2. Hand off to `InoNodes::Download::DownloadFileAsync`. The
     *      downloader internally handles:
     *        - Cache check (skip if file exists + SHA matches).
     *        - HEAD probe → GET → `.partial` staging → atomic rename.
     *        - Streaming SHA-256 verification against ExpectedSha256.
     *        - Multi-connection range downloads for large files.
     *        - Exponential-backoff retries on transient failures.
     *        - Cancellation via the subsystem's stored cancel token.
     *   3. On download success, call `litert_lm_engine_create` on a
     *      ThreadPool worker and marshal the result back to the game
     *      thread.
     *
     * Two per-call delegates (both single-cast dynamic delegates):
     *
     *   OnDownloadProgress — fires 0+ times on the game thread during
     *     download. Payload is the shared `FInoDownloadProgress` struct
     *     (BytesReceived / TotalBytes / ProgressPercent / BytesPerSecond
     *     / EstimatedSecondsRemaining / RetryAttempt / ...). If the model
     *     file is already cached on disk with a matching SHA, this
     *     delegate never fires.
     *
     *   OnLoaded — fires exactly ONCE at the end, with bSuccess=true
     *     when the engine is usable or bSuccess=false with an
     *     ErrorMessage when a terminal error prevented load.
     *
     * Error cases that fire OnLoaded(false, err):
     *   - Another load is already in flight.
     *   - A model is already loaded (call UnloadModel first).
     *   - No registry entry matches Config.ModelFileName.
     *   - The model file is missing and no DownloadUrl is configured.
     *   - Download failed (network, HTTP error, SHA-256 mismatch the
     *     downloader couldn't recover from).
     *   - litert_lm_engine_settings_create returned NULL.
     *   - litert_lm_engine_create returned NULL (corrupt model / out
     *     of memory).
     *
     * MUST be called on the game thread. Download + engine construction
     * run off-thread; all delegates marshal back to the game thread.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM",
              meta=(AutoCreateRefTerm="OnDownloadProgress,OnLoaded"))
    void LoadModelAsync(
        const FInoLiteRtLmModelConfig&              Config,
        const FInoLiteRtLmDownloadProgressDelegate& OnDownloadProgress,
        const FOnInoLiteRtLmModelLoaded&            OnLoaded);

    /**
     * True if LoadModelAsync has successfully completed and UnloadModel has
     * not yet been called. False during an in-flight load.
     */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsModelLoaded() const;

    /**
     * True if the named model is present on disk and non-empty, i.e.
     * LoadModelAsync would NOT need to download it before loading.
     *
     * Resolution order matches LoadModelAsync:
     *   1. PersistentDownloadDir/InoAgents/Models/  (auto-download cache)
     *   2. Plugins/InoAgents/Models/                (legacy dev drop)
     *
     * ModelNameOrFileName accepts either:
     *   - The on-disk filename ("gemma-4-E2B-it.litertlm"), OR
     *   - The DisplayName from Project Settings → Plugins → InoAgents →
     *     LiteRT-LM → Models ("Gemma 4 E2B"). Case-insensitive.
     *
     * Checks performed:
     *   - File exists at one of the two resolved locations.
     *   - File size > 0 (guards against zero-byte stubs).
     *
     * Does NOT perform SHA-256 verification — intentionally. Hashing a
     * multi-GB file costs seconds even on SSD and would be a terrible
     * thing to do synchronously on the game thread. The SHA-256 check
     * happens asynchronously inside LoadModelAsync when the entry has
     * an ExpectedSha256 configured; this method is only a cheap "is it
     * on disk" probe, useful for deciding whether to show a download
     * progress UI before the user kicks off a load.
     *
     * Pure — safe to call from Blueprint constant-evaluated contexts,
     * Tick, or any thread (file I/O is read-only stat).
     */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsModelDownloaded(const FString& ModelNameOrFileName) const;

    /**
     * Destroy the loaded engine. Safe to call with no model loaded (no-op).
     *
     * If an active conversation exists (the one returned by the most recent
     * CreateConversation call and still alive), UnloadModel calls Shutdown()
     * on it first so its native LiteRtLmConversation is destroyed before
     * the engine it references. Without this ordering, the conversation's
     * ~SessionBasic destructor would dereference freed engine memory when
     * GC eventually reclaims the conversation UObject.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void UnloadModel();

    // ------------------------------------------------------------------
    // Conversation factory
    // ------------------------------------------------------------------

    /**
     * Create and return a new UInoLiteRtLmConversation bound to the currently
     * loaded engine. Returns nullptr if no model is loaded.
     *
     * Single-conversation enforcement: LiteRT-LM does not support two live
     * sessions on the same engine (they share one LlmExecutor and KV cache).
     * If a prior conversation created by this subsystem is still alive,
     * CreateConversation calls Shutdown() on it first and logs a warning.
     * Any in-flight stream on the prior conversation is cancelled. After
     * Shutdown() the prior conversation is a zombie — SendMessageAsync will
     * error — but its UObject remains valid until the caller drops their
     * reference and GC collects it.
     *
     * The subsystem does NOT own the returned conversation — the caller
     * must hold a reference (UPROPERTY on an actor, widget, or other
     * UObject) to keep it alive. The subsystem only keeps a TWeakObjectPtr
     * so it can enforce the single-conversation invariant and perform
     * ordered teardown at UnloadModel / Deinitialize time.
     *
     * Uses the currently-loaded config (LoadedConfig) for system message
     * and backend. A future iteration may add an OverrideConfig parameter
     * for per-conversation customization.
     *
     * The conversation is created with a snapshot of the currently
     * registered tools (see RegisterTool below). Tools registered AFTER
     * the conversation is created do not retroactively apply.
     */
    /** Create a conversation with no initial history. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    UInoLiteRtLmConversation* CreateConversation();

    /** Create a conversation pre-populated with saved history. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM",
              meta = (DisplayName = "Create Conversation With History"))
    UInoLiteRtLmConversation* CreateConversationWithHistory(
        const TArray<FInoLiteRtLmMessage>& InitialMessages);

    // ------------------------------------------------------------------
    // Tool registry
    // ------------------------------------------------------------------

    /**
     * Register a tool so it becomes available to every conversation
     * created AFTER this call. Registering the same tool name again
     * overwrites the previous registration and logs a warning.
     *
     * The subsystem validates the tool's schema (built from its
     * ToolName, Description, and Parameters properties) at registration
     * time. Tools with empty names or unparseable schemas are rejected.
     *
     * Conversations that were created before RegisterTool ran do NOT
     * see the new tool — tools_json is snapshotted at conversation
     * creation time.
     *
     * Called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Tools")
    void RegisterTool(UInoLiteRtLmToolBase* Tool);

    /**
     * Remove a tool from the registry by name. If no tool is registered
     * under that name this is a no-op and logs at Verbose level.
     * Conversations already created continue to see the tool they were
     * constructed with — removing a tool does not retroactively affect
     * live conversations.
     *
     * Called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Tools")
    void UnregisterTool(FName ToolName);

    /**
     * Look up a tool by name. Returns nullptr if no tool is registered
     * under that name. Called by UInoLiteRtLmConversation's worker agent
     * loop on the game thread when the model emits a tool call.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="InoAgents|LiteRT-LM|Tools")
    UInoLiteRtLmToolBase* FindTool(FName ToolName) const;

    /**
     * Serialise every registered tool's schema into a single JSON
     * array, ready to be passed as the `tools_json` argument to
     * litert_lm_conversation_config_create. Returns an empty string
     * if no tools are registered (which causes CreateConversation to
     * skip the tools_json arg entirely, disabling tool calling for
     * that conversation).
     *
     * Called once per CreateConversation call, on the game thread.
     * Schemas are re-serialised each time rather than cached because
     * RegisterTool is rare and the O(Tools.Num()) cost is trivial.
     */
    FString BuildToolsJsonForConversation() const;

private:
    // Opaque native handle. Never exposed to Blueprint. The extern "C"
    // forward declaration at the top of this file makes this type-check
    // without including the LiteRT-LM C header.
    //
    // We deliberately do NOT keep a LiteRtLmEngineSettings* alive past
    // engine_create — the settings are consumed synchronously inside
    // EngineFactory::CreateDefault (see vendor/LiteRT-LM/c/engine.cc:471-488)
    // and the resulting Engine carries everything it needs forward. The
    // settings handle is freed on the worker thread immediately after
    // engine_create returns.
    LiteRtLmEngine* Engine = nullptr;

    /** Snapshot of the config used for the most recent successful load.
     *  Used by CreateConversation to read SystemMessage, etc. */
    FInoLiteRtLmModelConfig LoadedConfig;

    // True from the moment LoadModelAsync dispatches to the InoNodes
    // downloader (or the ThreadPool engine_create when the file is
    // cached) until the OnLoaded callback fires back on the game thread.
    bool bLoadInFlight = false;

    // Per-call delegates stashed here for the duration of the load so
    // every progress tick + the terminal OnLoaded all fire through the
    // caller's delegates. Cleared on terminal completion so a stale
    // delegate from a prior load can't be invoked against a fresh load.
    FOnInoLiteRtLmModelLoaded            PendingOnLoaded;
    FInoLiteRtLmDownloadProgressDelegate PendingOnDownloadProgress;

    // Cancellation token for the in-flight InoNodes download. Created
    // when LoadModelAsync starts a download; nulled when the download
    // completes; cancelled in Deinitialize so the downloader stops
    // writing to disk if the subsystem tears down mid-load.
    FInoCancellationTokenPtr DownloadCancelToken;

    /**
     * Once the file is on disk and verified by the InoNodes downloader,
     * hand off to ThreadPool for `litert_lm_engine_create` and marshal
     * the result back to the game thread via the stored
     * PendingOnLoaded delegate. Game thread only.
     */
    void DispatchModelLoad(const FString& ModelPath);

    // Weak ref to the most recently created conversation. Used to enforce
    // the single-conversation invariant and to tear the conversation down
    // in the correct order at UnloadModel/Deinitialize time (before the
    // engine is destroyed). Weak so the subsystem does not keep the
    // conversation alive — callers own that decision via their own UPROPERTY
    // references.
    TWeakObjectPtr<UInoLiteRtLmConversation> ActiveConversation;

    // Tool registry. Keyed by the tool's ToolName. UPROPERTY keeps
    // the tool alive while registered. UnregisterTool drops the
    // reference; Deinitialize clears the whole map.
    UPROPERTY()
    TMap<FName, TObjectPtr<UInoLiteRtLmToolBase>> Tools;
};
