// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ScriptInterface.h"

#include "LiteRtLm/LiteRtLmTool.h"    // TScriptInterface<ILiteRtLmTool> needs full type
#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmSubsystem.generated.h"

class ULiteRtLmModelConfig;
class ULiteRtLmConversation;

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

    // ------------------------------------------------------------------
    // Conversation factory (D.2)
    // ------------------------------------------------------------------

    /**
     * Create and return a new ULiteRtLmConversation bound to the currently
     * loaded engine. Returns nullptr if no model is loaded.
     *
     * The subsystem does NOT own the returned conversation — the caller
     * must hold a reference (UPROPERTY on an actor, widget, or other
     * UObject) to keep it alive. When no references remain, UE garbage
     * collection destroys the conversation, which joins its worker thread
     * and releases native resources.
     *
     * Uses the currently-loaded config (LoadedConfig) for system message
     * and backend. A future iteration may add an OverrideConfig parameter
     * for per-conversation customization.
     *
     * The conversation is created with a snapshot of the currently
     * registered tools (see RegisterTool below). Tools registered AFTER
     * the conversation is created do not retroactively apply.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    ULiteRtLmConversation* CreateConversation();

    // ------------------------------------------------------------------
    // Tool registry (D.4)
    // ------------------------------------------------------------------

    /**
     * Register a tool implementation so it becomes available to every
     * conversation created AFTER this call. Registering the same tool
     * name again overwrites the previous registration and logs a
     * warning.
     *
     * The subsystem parses the tool's schema JSON once at registration
     * time and verifies that its "function.name" field matches the
     * tool's GetToolName(). Tools with mismatched or unparseable
     * schemas are rejected and logged, not silently accepted.
     *
     * Conversations that were created before RegisterTool ran do NOT
     * see the new tool — tool_json is snapshotted at conversation
     * creation time because LiteRT-LM's tools_json is passed to
     * conversation_config_create and not mutable afterward.
     *
     * Called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Tools")
    void RegisterTool(TScriptInterface<ILiteRtLmTool> Tool);

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
     * under that name. Called by ULiteRtLmConversation's worker agent
     * loop on the game thread when the model emits a tool call.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="InoAgents|LiteRT-LM|Tools")
    TScriptInterface<ILiteRtLmTool> FindTool(FName ToolName) const;

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

    // Kept alive while the model is loaded so that GC does not reclaim
    // the asset out from under us.
    UPROPERTY()
    TObjectPtr<const ULiteRtLmModelConfig> LoadedConfig;

    // True from the moment LoadModelAsync dispatches to the ThreadPool
    // until the OnLoaded callback fires back on the game thread.
    bool bLoadInFlight = false;

    // Tool registry. Keyed by the tool's GetToolName() result. Values
    // are TScriptInterface<ILiteRtLmTool>, which keeps a UPROPERTY
    // reference to the implementing UObject so the tool is not
    // garbage-collected while registered. UnregisterTool drops the
    // reference; Deinitialize clears the whole map.
    UPROPERTY()
    TMap<FName, TScriptInterface<ILiteRtLmTool>> Tools;
};
