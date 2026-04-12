// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"  // full type needed for UPROPERTY TSet<TObjectPtr<>>
#include "Subsystems/GameInstanceSubsystem.h"

#include "ElevenLabs/ElevenLabsTypes.h"

#include "ElevenLabsSubsystem.generated.h"

/**
 * Game-instance-wide ElevenLabs HTTP client state.
 *
 * Not a front-door API - callers still use the per-endpoint async action
 * (UElevenLabsTextToDialogueStream and, in future phases, TTS / STT
 * equivalents). This subsystem exists for three reasons:
 *
 *   1. Cache settings once per PIE session so every call doesn't have to
 *      walk GetDefault<UElevenLabsSettings>().
 *   2. Anchor in-flight async actions' GC lifetimes via a UPROPERTY set,
 *      replacing the AddToRoot pattern that each action would otherwise
 *      need to stay alive across its HTTP round-trip.
 *   3. Provide a single CancelAll() path that fires at Deinitialize() so
 *      HTTP responses that arrive after PIE ends can't dispatch into freed
 *      UObjects. Without this, a long-running dialogue stream kicked off
 *      mid-game would keep running under the now-dead world's game thread
 *      and crash when OnComplete tried to broadcast.
 *
 * Scope: shared across all ElevenLabs endpoints (phase 1 + future phases
 * 2 and 3). No LiteRT-LM coupling.
 */
UCLASS(BlueprintType)
class INOAGENTS_API UElevenLabsSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    //~ UGameInstanceSubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem interface

    // -----------------------------------------------------------------
    // Cached settings accessors
    // -----------------------------------------------------------------

    /** xi-api-key snapshotted from UElevenLabsSettings at Initialize /
     *  ReloadSettings time. Never logged verbatim. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|ElevenLabs")
    FString GetApiKey() const { return CachedApiKey; }

    /** Effective base URL (override or hardcoded default). Always a full
     *  https://... with no trailing slash. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|ElevenLabs")
    FString GetBaseUrl() const { return CachedBaseUrl; }

    /** Default model id used when a per-call request leaves ModelId empty. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|ElevenLabs")
    FString GetDefaultModelId() const { return CachedDefaultModelId; }

    /** Default audio format for callers that don't override it. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|ElevenLabs")
    EElevenLabsOutputFormat GetDefaultOutputFormat() const { return CachedDefaultOutputFormat; }

    /**
     * Re-reads UElevenLabsSettings into the cached fields without
     * restarting PIE. Exposed to Blueprint and to the
     * InoAgents.ElevenLabs.ReloadSettings console command so devs can
     * iterate on API keys during a running session.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|ElevenLabs")
    void ReloadSettings();

    // -----------------------------------------------------------------
    // In-flight request registry / cancellation
    // -----------------------------------------------------------------

    /**
     * Cancel every in-flight ElevenLabs request tracked by the subsystem.
     *
     * Called automatically from Deinitialize() at PIE end. Safe to call
     * manually (e.g. from a pause menu that wants to abort any pending
     * TTS) and safe to call with no active requests. Idempotent.
     *
     * Each live action receives OnError("cancelled") before its
     * UPROPERTY reference is dropped.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|ElevenLabs")
    void CancelAll();

    /**
     * Register an async action so the subsystem keeps it alive (via its
     * LiveActions UPROPERTY set) and so CancelAll() can reach it. Called
     * by the action from its Activate(), immediately before it dispatches
     * its HTTP request.
     *
     * Not BlueprintCallable - this is an internal contract between the
     * subsystem and its sibling async action classes.
     */
    void RegisterLiveAction(UBlueprintAsyncActionBase* Action);

    /**
     * Drop the subsystem's strong reference to an async action. Called
     * from the action's FinishCleanly() after OnComplete/OnError has
     * broadcast. After this call returns, the action is eligible for GC
     * on the next pass unless something else (like a Blueprint node or a
     * user UPROPERTY) still holds it.
     *
     * Not BlueprintCallable.
     */
    void UnregisterLiveAction(UBlueprintAsyncActionBase* Action);

private:
    /** Cached settings snapshot. Repopulated by ReloadSettings(). */
    FString                 CachedApiKey;
    FString                 CachedBaseUrl;
    FString                 CachedDefaultModelId;
    EElevenLabsOutputFormat CachedDefaultOutputFormat = EElevenLabsOutputFormat::Mp3_44100_128;

    /**
     * Strong references to every in-flight async action. A TSet of
     * TObjectPtr is a UPROPERTY so the GC keeps each action alive as
     * long as the subsystem has a reference, without needing each
     * action to AddToRoot itself. Actions unregister in FinishCleanly().
     */
    UPROPERTY()
    TSet<TObjectPtr<UBlueprintAsyncActionBase>> LiveActions;
};
