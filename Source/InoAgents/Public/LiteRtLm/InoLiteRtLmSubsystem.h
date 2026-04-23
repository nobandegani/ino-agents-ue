// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ScriptInterface.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"

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
    struct LiteRtLmEngineSettings;
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
class INOAGENTS_API UInoLiteRtLmSubsystem : public UGameInstanceSubsystem
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
     * Returns immediately. When loading finishes (success or failure),
     * OnLoaded fires on the game thread.
     *
     * Flow:
     *   1. Resolve the model on disk (LiteRtLmResolveModelPath).
     *   2. If missing → download from the model entry's DownloadUrl into
     *      PersistentDownloadDir.
     *   3. If the entry carries an ExpectedSha256, SHA-256 the file on a
     *      ThreadPool worker before loading. On mismatch:
     *        - Cached file: delete and re-download, then verify again.
     *        - Freshly-downloaded file: hard fail (OnLoaded false) — no
     *          redownload loop.
     *   4. Call litert_lm_engine_create on the ThreadPool worker and
     *      marshal the result back to the game thread.
     *
     * Error cases that fire OnLoaded with bSuccess=false:
     *   - Another load is already in flight
     *   - A model is already loaded (call UnloadModel first)
     *   - The model file does not exist and no DownloadUrl is configured
     *   - SHA-256 verification failed and cannot be recovered (no URL, or
     *     a fresh download also mismatched)
     *   - litert_lm_engine_settings_create returned NULL
     *   - litert_lm_engine_create returned NULL (corrupt / unsupported model)
     *
     * MUST be called on the game thread. The actual SHA-256 + engine
     * construction run on ThreadPool workers; the OnLoaded callback
     * marshals back to the game thread.
     */
    /**
     * Dispatch a model load.
     *
     * Two per-call delegates (both are single-cast dynamic delegates,
     * same ergonomic as OnLoaded before this — they show as exec-pin
     * Events on the BP node when AutoCreateRefTerm fires, and in C++
     * you bind one handler per call via BindDynamic):
     *
     *   OnDownloadProgress — fires 0+ times during download only. If
     *     the model file is already cached on disk, this never fires.
     *     Payload: (Percent, BytesReceived, TotalBytes, bCompleted).
     *     bCompleted=false on every intermediate tick; bCompleted=true
     *     on exactly ONE terminal tick, fired AFTER the download is
     *     fully written + renamed on disk, BEFORE the SHA-256 verify
     *     + engine construction begin. Use it to flip UI from
     *     "downloading" to "loading" without waiting for OnLoaded.
     *
     *   OnLoaded — fires exactly ONCE at the end, when the engine is
     *     actually usable (bSuccess=true) or a terminal error
     *     prevented load (bSuccess=false, ErrorMessage filled).
     *
     * On download failure the OnDownloadProgress.bCompleted=true is
     * NOT fired — the error flows through OnLoaded(false, err).
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM",
              meta=(AutoCreateRefTerm="OnDownloadProgress,OnLoaded"))
    void LoadModelAsync(
        const FInoLiteRtLmModelConfig&         Config,
        const FOnInoModelDownloadProgress&     OnDownloadProgress,
        const FOnInoLiteRtLmModelLoaded&       OnLoaded);

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
    // Chat panel (dev/debug UI)
    // ------------------------------------------------------------------

    /**
     * Show the in-PIE Slate chat panel, connected to a conversation the
     * caller already owns. The panel uses the conversation's existing
     * delegates (OnToken, OnComplete, OnError, OnToolCalled) to display
     * streaming tokens, tool-call pills, and errors.
     *
     * If Conversation is null, the subsystem creates a new one internally
     * (same behaviour as the Ino.LiteRtLm.ShowChatPanel console
     * command). If a panel is already showing, it is torn down first.
     *
     * The subsystem does NOT take ownership of the conversation — the
     * caller must keep it alive as long as the panel is visible. If the
     * conversation is GC'd while the panel is open, the bridge's weak-
     * ptr-based handlers silently no-op and the panel stays on screen
     * in a "disconnected" state until HideChatPanel is called.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|LiteRT-LM|UI")
    void ShowChatPanel(UInoLiteRtLmConversation* Conversation = nullptr);

    /**
     * Tear down the chat panel shown by ShowChatPanel. Safe to call when
     * no panel is visible (no-op). Also called automatically at PIE end
     * via the PrePIEEnded hook.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|LiteRT-LM|UI")
    void HideChatPanel();

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
    // Opaque native handles. Never exposed to Blueprint. The extern "C"
    // forward declarations at the top of this file make these type-check
    // without including the LiteRT-LM C header.
    LiteRtLmEngine*          Engine   = nullptr;
    LiteRtLmEngineSettings*  Settings = nullptr;

    /** Snapshot of the config used for the most recent successful load.
     *  Used by CreateConversation to read SystemMessage, etc. */
    FInoLiteRtLmModelConfig LoadedConfig;

    // True from the moment LoadModelAsync dispatches to the ThreadPool
    // until the OnLoaded callback fires back on the game thread.
    bool bLoadInFlight = false;

    // Download state — chunked Range-based download to avoid UE's
    // HTTP module accumulating the full response in a TArray<uint8>
    // (which overflows int32 at ~2.1 GB and crashes for 3+ GB models).
    // Each chunk is <=500 MB, safely within TArray limits.
    static constexpr int64 kDownloadChunkSize = 500 * 1024 * 1024;  // 500 MB

    FString         DownloadUrl;
    FString         PendingDownloadTargetPath;
    FOnInoLiteRtLmModelLoaded        PendingOnLoaded;
    /** Per-call download-progress handler stashed here for the
     *  duration of the load so every progress tick (including the
     *  bCompleted=true terminal tick) fires through the caller's
     *  delegate. Cleared on terminal completion so a stale delegate
     *  from a prior load can't be invoked against a fresh request. */
    FOnInoModelDownloadProgress      PendingOnDownloadProgress;
    IFileHandle*    DownloadFileHandle = nullptr;
    int64           DownloadBytesWritten = 0;
    // Full file size in bytes, as learned from the first response's
    // Content-Range (for 206 Partial Content) or Content-Length (for 200 OK).
    // -1 means "not yet known" or "server didn't advertise it" — in which
    // case OnDownloadProgress fires with Percent=0 and TotalBytes=-1.
    int64           DownloadTotalBytes = -1;
    FHttpRequestPtr DownloadRequest;

    void StartDownload(const FString& Url, const FString& TargetPath,
                       const FOnInoLiteRtLmModelLoaded& OnLoaded);
    void DownloadNextChunk();
    void HandleChunkComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void FinishDownloadSuccess();
    void FinishDownloadError(const FString& Error);
    void CleanupDownload();
    void ProceedWithLoad(const FString& ModelPath, const FOnInoLiteRtLmModelLoaded& OnLoaded);

    /**
     * Verify the file at ModelPath against the entry's ExpectedSha256 (if set),
     * then either:
     *   - Hand off to ProceedWithLoad on a match (or if verification is skipped
     *     because the entry has no expected hash).
     *   - On mismatch: delete the file and, if bAllowRedownloadOnMismatch is
     *     true and the entry has a DownloadUrl, call StartDownload with the
     *     same OnLoaded delegate. Otherwise fail via the OnLoaded delegate
     *     without retrying (used from FinishDownloadSuccess to avoid infinite
     *     retry loops on persistently-bad downloads).
     *
     * SHA-256 computation is dispatched to the ThreadPool. Result dispatch
     * back to the game thread is done via AsyncTask(ENamedThreads::GameThread).
     * A TWeakObjectPtr<UInoLiteRtLmSubsystem> guards against teardown
     * mid-verification — if the subsystem is gone when the result arrives,
     * the lambda no-ops.
     *
     * MUST be called on the game thread.
     */
    void VerifyAndLoad(const FString& ModelPath,
                       const FInoLiteRtLmModelEntry* Entry,
                       const FOnInoLiteRtLmModelLoaded& OnLoaded,
                       bool bAllowRedownloadOnMismatch);

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
