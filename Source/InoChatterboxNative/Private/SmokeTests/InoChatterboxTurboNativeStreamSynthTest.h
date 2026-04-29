// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ChatterboxTurboNative/InoChatterboxTurboNativeTypes.h"

#include "InoChatterboxTurboNativeStreamSynthTest.generated.h"

class UInoChatterboxTurboNativeSubsystem;
class UInoChatterboxTurboNativeStreamSynthesize;

/**
 * Observer UCLASS for the Ino.Chatterbox.StreamSynthTest console
 * command. Mirrors UInoChatterboxTurboNativeSubsystemTestObserver but drives the
 * UInoChatterboxTurboNativeStreamSynthesize async action instead of calling
 * SynthesizeAsync directly — so it exercises both the subsystem's
 * SynthesizeStreamAsync path AND the Blueprint-facing async action's
 * delegate forwarding.
 *
 * Binds the async action's three multicast delegates (OnAudioChunk,
 * OnComplete, OnError) plus the subsystem's OnLoaded. GC-safety is
 * AddToRoot at start / RemoveFromRoot on terminal event, same as the
 * non-streaming subsystem test.
 *
 * Accumulates all OnAudioChunk bytes into a local TArray<uint8> and
 * verifies at OnComplete that the accumulated bytes byte-equal the
 * Result.AudioSamples — catches most "runner dropped a chunk" /
 * "stream/final delta miscomputed" regressions in one check.
 *
 * Dynamic-delegate handler quirks: FString by value, TArray<uint8>
 * by const ref — matches the delegate declarations in
 * InoChatterboxTurboNativeStreamSynthesize.h exactly.
 */
UCLASS()
class UInoChatterboxTurboNativeStreamSynthTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;

    /** Start-of-stream wallclock. Rebased when OnLoaded fires so
     *  first-chunk latency measures only the actual synth dispatch. */
    double SynthStartTime = 0.0;

    /** Ticks each time OnAudioChunk fires. Logged for diagnostics and
     *  used to compute first-chunk latency. */
    int32  ChunksReceived = 0;

    /** Logged on first chunk only — shows how long it took from
     *  SynthesizeStreamAsync call to first audio hitting the callback. */
    double FirstChunkMs = 0.0;

    /** Running concatenation of every OnAudioChunk's bytes. Compared to
     *  Result.AudioSamples at OnComplete for round-trip integrity. */
    TArray<uint8> AccumulatedBytes;

    UPROPERTY()
    TObjectPtr<UInoChatterboxTurboNativeSubsystem> Subsystem = nullptr;

    /** Strong ref on the live async action so GC doesn't collect it out
     *  from under us if the LatentActionManager path fails to hold it
     *  (belt-and-braces; RegisterWithGameInstance already does this). */
    UPROPERTY()
    TObjectPtr<UInoChatterboxTurboNativeStreamSynthesize> Action = nullptr;

    FString VoiceWavPath;
    FString PromptText;
    FString OutputWavPath;
    int32   MaxNewTokens      = 512;
    int32   StreamChunkTokens = 20;

    /** Hop-in from the subsystem's LoadModelsAsync. Builds the async
     *  action + binds its three delegates + calls Activate. */
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

    /** OnAudioChunk forwarder — logs chunk size, accumulates bytes. */
    UFUNCTION()
    void HandleChunk(const TArray<uint8>& AudioChunk, bool bIsFinal, int32 NumGeneratedTokens);

    /** OnComplete forwarder — runs integrity checks, writes the WAV,
     *  unloads the subsystem, removes from root. */
    UFUNCTION()
    void HandleComplete(bool bSuccess,
                        FInoChatterboxTurboNativeSynthesisResult Result,
                        FString                       ErrorMessage);

    /** OnError forwarder — logs + tears down. Mutually exclusive with
     *  HandleComplete success. */
    UFUNCTION()
    void HandleError(FString ErrorMessage);

    /** Shared teardown path used by HandleComplete + HandleError.
     *  Unloads the subsystem and unroots. Idempotent. */
    void Teardown();
};
