// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "InoNeuTtsTypes.h"

#include "InoNeuTtsStreamSynthesize.generated.h"

/**
 * Latent Blueprint node "NeuTTS Synthesize Streaming" — wraps
 * UInoNeuTtsSubsystem::SynthesizeStreamAsync as an exec-pin async action
 * with OnAudioChunk, OnComplete, and OnError pins. Each chunk fired on
 * OnAudioChunk is 24 kHz mono int16 PCM LE bytes — feed straight into
 * UStreamingSoundWave::AppendAudioDataFromRAW (RuntimeAudioImporter).
 *
 * Voice is set separately via SetActiveVoiceAsync — this node always
 * synthesises with whatever voice is currently primed on the subsystem.
 * Errors fast through OnError if no active voice has been set.
 */
UCLASS()
class INONEUTTSNATIVE_API UInoNeuTtsStreamSynthesize : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/**
	 * Fires for every emitted chunk during streaming. Last invocation
	 * has bIsFinal=true. Driving the audio pipeline directly from this
	 * pin gives the lowest latency.
	 */
	UPROPERTY(BlueprintAssignable)
	FOnInoNeuTtsAudioChunk OnAudioChunk;

	/** Fires once after the final chunk, with the concatenated full waveform. */
	UPROPERTY(BlueprintAssignable)
	FOnInoNeuTtsSynthesisComplete OnComplete;

	/** Fires once on any failure (cancel, missing model, missing voice, etc). */
	UPROPERTY(BlueprintAssignable)
	FOnInoNeuTtsSynthesisComplete OnError;

	/**
	 * Static Blueprint factory.
	 *
	 * @param ChunkTokens  Number of codec frames per emitted chunk.
	 *                     0 uses the default of 25 (~0.5 s at 24 kHz).
	 *                     Smaller = lower first-audio latency, more
	 *                     decoder runs.
	 */
	UFUNCTION(BlueprintCallable, BlueprintInternalUseOnly,
		Category = "InoNeuTts",
		meta = (DisplayName = "NeuTTS Synthesize Streaming",
		        BlueprintInternalUseOnly = "true",
		        WorldContext = "WorldContextObject"))
	static UInoNeuTtsStreamSynthesize* SynthesizeStreamAsync(
		UObject* WorldContextObject,
		const FString& Text,
		const FInoNeuTtsOptions& Options,
		int32 ChunkTokens = 25);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleChunk(const TArray<uint8>& ChunkBytes, bool bIsFinal);

	UFUNCTION()
	void HandleComplete(const FInoNeuTtsResult& Result);

	UPROPERTY()
	TWeakObjectPtr<UObject> WorldContextObjectPtr;

	UPROPERTY()
	FString StoredText;

	UPROPERTY()
	FInoNeuTtsOptions StoredOptions;

	UPROPERTY()
	int32 StoredChunkTokens = 25;
};
