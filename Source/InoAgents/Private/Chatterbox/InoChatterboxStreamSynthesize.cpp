// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Chatterbox/InoChatterboxStreamSynthesize.h"

#include "Chatterbox/InoChatterboxTtsSubsystem.h"
#include "InoAgentsLog.h"

#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UInoChatterboxStreamSynthesize* UInoChatterboxStreamSynthesize::StreamSynthesize(
    UObject*                              WorldContextObject,
    const FString&                        Text,
    const FInoChatterboxVoice&            Voice,
    const FInoChatterboxSynthesisOptions& Options,
    int32                                 StreamChunkTokens)
{
    UInoChatterboxStreamSynthesize* Action = NewObject<UInoChatterboxStreamSynthesize>();
    Action->PendingText              = Text;
    Action->PendingVoice             = Voice;
    Action->PendingOptions           = Options;
    Action->PendingStreamChunkTokens = FMath::Max(0, StreamChunkTokens);
    Action->WorldContextObjectWeak   = WorldContextObject;

    // Registering with the game instance parks the action under UE's
    // LatentActionManager, which holds a strong ref until
    // SetReadyToDestroy() runs. Without this, the async action would be
    // eligible for GC immediately after the factory returns and
    // Activate() would never get called.
    //
    // No-op when WorldContextObject is null (e.g. called from a console
    // command with no owning actor) — in that case the smoke-test caller
    // is expected to AddToRoot() the action manually.
    if (WorldContextObject != nullptr)
    {
        Action->RegisterWithGameInstance(WorldContextObject);
    }

    return Action;
}

// ---------------------------------------------------------------------------
// Activate
// ---------------------------------------------------------------------------

void UInoChatterboxStreamSynthesize::Activate()
{
    // 1. Resolve the subsystem via the captured WorldContextObject.
    UInoChatterboxTtsSubsystem* Subsystem = nullptr;
    if (UObject* Ctx = WorldContextObjectWeak.Get())
    {
        if (UGameInstance* GI = UGameplayStatics::GetGameInstance(Ctx))
        {
            Subsystem = GI->GetSubsystem<UInoChatterboxTtsSubsystem>();
        }
    }
    if (Subsystem == nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoChatterboxStreamSynthesize::Activate: no subsystem ")
               TEXT("(need active GameInstance — PIE or packaged)"));
        OnError.Broadcast(TEXT("No UInoChatterboxTtsSubsystem — "
                               "call from a live game instance (PIE or packaged)"));
        FinishCleanly();
        return;
    }
    SubsystemWeak = Subsystem;

    // 2. Bind our internal forwarders to the subsystem's dynamic
    //    delegates. Single-cast dynamic — so each delegate variable
    //    holds exactly one function pointer pair; we're the sole
    //    subscriber for this particular SynthesizeStreamAsync call.
    FOnInoChatterboxAudioChunk OnChunkBound;
    OnChunkBound.BindDynamic(this, &UInoChatterboxStreamSynthesize::HandleChunk);

    FOnInoChatterboxSynthesisComplete OnCompleteBound;
    OnCompleteBound.BindDynamic(this, &UInoChatterboxStreamSynthesize::HandleComplete);

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoChatterboxStreamSynthesize::Activate: submitting text_len=%d ")
           TEXT("max_tokens=%d stream_chunk_tokens=%d"),
           PendingText.Len(), PendingOptions.MaxNewTokens,
           PendingStreamChunkTokens);

    // 3. Submit. Subsystem's pre-flight (no models loaded, bad voice,
    //    empty text, etc.) fires OnCompleteBound synchronously with
    //    bSuccess=false — which lands in HandleComplete and routes to
    //    OnError, so the BP flow unwinds cleanly without special-casing.
    Subsystem->SynthesizeStreamAsync(
        PendingText, PendingVoice, PendingOptions,
        OnChunkBound, OnCompleteBound,
        PendingStreamChunkTokens);
}

// ---------------------------------------------------------------------------
// Subsystem delegate forwarders
// ---------------------------------------------------------------------------

void UInoChatterboxStreamSynthesize::HandleChunk(
    const TArray<uint8>& AudioChunk, bool bIsFinal, int32 NumGeneratedTokens)
{
    // Late chunks after cancel / error shouldn't re-enter.
    if (bFinished)
    {
        return;
    }
    OnAudioChunk.Broadcast(AudioChunk, bIsFinal, NumGeneratedTokens);
}

void UInoChatterboxStreamSynthesize::HandleComplete(
    bool                          bSuccess,
    FInoChatterboxSynthesisResult Result,
    FString                       ErrorMessage)
{
    if (bFinished)
    {
        return;
    }

    if (bSuccess)
    {
        OnComplete.Broadcast(true, Result, FString());
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoChatterboxStreamSynthesize: synthesis failed: %s"),
               *ErrorMessage);
        OnError.Broadcast(ErrorMessage);
    }
    FinishCleanly();
}

// ---------------------------------------------------------------------------
// Cancel / teardown
// ---------------------------------------------------------------------------

void UInoChatterboxStreamSynthesize::Cancel()
{
    if (bFinished)
    {
        return;
    }
    // We don't reach into the subsystem to abort — its worker is shared
    // across all in-flight synth items and we don't have a per-item
    // handle. Users who genuinely need to free the worker's CPU call
    // UInoChatterboxTtsSubsystem::CancelSynthesis instead; this
    // Cancel() is a lightweight "I'm no longer listening" signal from
    // the Blueprint side.
    OnError.Broadcast(TEXT("cancelled"));
    FinishCleanly();
}

void UInoChatterboxStreamSynthesize::FinishCleanly()
{
    if (bFinished)
    {
        return;
    }
    bFinished = true;

    // Release the LatentActionManager's strong ref so GC can reclaim us
    // once any remaining delegate dispatch frames unwind.
    SetReadyToDestroy();
}
