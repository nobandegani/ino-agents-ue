// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "NeuTTS/InoNeuTTSTypes.h"

#include "InoNeuTTSStreamSynthesize.generated.h"

/**
 * Blueprint async-action wrapper for
 * `UInoNeuTTSSubsystem::SynthesizeStreamAsync`.
 *
 * Displays in the Blueprint context menu as
 * **"NeuTTS Synthesize Streaming"** with three exec pins:
 * `OnAudioChunk`, `OnComplete`, `OnError`.
 *
 * In the current MVP, exactly one OnAudioChunk fires with the full PCM
 * and `bIsFinal=true`, followed by OnComplete. Vendor's chunked
 * overlap-add streaming is deferred — see `InoNeuTTSSynthesisWorker.h`
 * for the design notes.
 */
UCLASS()
class INONEUTTS_API UInoNeuTTSStreamSynthesize : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /** Fires per emitted chunk. `bIsFinal=true` on the last chunk. */
    UPROPERTY(BlueprintAssignable)
    FOnInoNeuTTSAudioChunk OnAudioChunk;

    /** Fires once at terminal success with the concatenated full waveform. */
    UPROPERTY(BlueprintAssignable)
    FOnInoNeuTTSSynthesisComplete OnComplete;

    /** Fires once at terminal failure. */
    UPROPERTY(BlueprintAssignable)
    FOnInoNeuTTSSynthesisComplete OnError;

    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext = "WorldContextObject",
                      DisplayName  = "NeuTTS Synthesize Streaming"))
    static UInoNeuTTSStreamSynthesize* SynthesizeStreamAsync(
        UObject* WorldContextObject,
        const FString& Text,
        const FInoNeuTTSOptions& Options,
        int32 ChunkTokens = 25);

    virtual void Activate() override;

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject_ = nullptr;

    FString           Text_;
    FInoNeuTTSOptions Options_;
    int32             ChunkTokens_ = 25;

    UFUNCTION()
    void HandleChunk(const TArray<uint8>& AudioChunk, bool bIsFinal);

    UFUNCTION()
    void HandleComplete(const FInoNeuTTSResult& Result);
};
