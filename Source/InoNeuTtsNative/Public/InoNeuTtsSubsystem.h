// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SharedPointer.h"

#include "InoNeuTtsTypes.h"

#include <atomic>

#include "InoNeuTtsSubsystem.generated.h"

namespace InoNeuTtsNative
{
	class FInoNeuTtsRunner;
}

/**
 * Game-instance-scoped subsystem driving NeuTTS Nano + Air synthesis.
 *
 * Lifecycle (typical):
 *   1. LoadModelAsync(Config, OnLoaded)        // off-thread, fires OnLoaded on game thread
 *   2. ListBundledVoices() / LoadVoiceFromFile  // build / pick a voice
 *   3. SynthesizeAsync(Text, Voice, Options, OnComplete)
 *   4. (optionally CancelSynthesis() to abort an in-flight call)
 *   5. UnloadModel()                            // releases GGUF + ONNX
 *
 * Threading invariants:
 *   - All public methods MUST be called from the game thread.
 *   - Delegates fire on the game thread.
 *   - The runner is held via TSharedPtr so an in-flight synth on a
 *     worker thread keeps it alive even if the subsystem releases its
 *     own reference (UnloadModel during a synth is safe — the synth
 *     finishes and the runner frees only after the worker exits).
 *   - Only one synth at a time is allowed in v1; SynthesizeAsync errors
 *     if a previous call hasn't fired its OnComplete yet.
 */
UCLASS()
class INONEUTTSNATIVE_API UInoNeuTtsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// ---- USubsystem ----
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- Model lifecycle ----

	/**
	 * Load the GGUF backbone + ONNX decoder for the configured variant.
	 * No-op (with a warning) if a model is already loaded; call
	 * UnloadModel first to switch variants.
	 *
	 * Dispatches to the UE thread pool so the game thread stays
	 * responsive during the multi-second model mmap. OnLoaded fires
	 * on the game thread regardless of success.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void LoadModelAsync(
		const FInoNeuTtsConfig& Config,
		const FInoNeuTtsLoadedDelegate& OnLoaded);

	/**
	 * Drop the loaded model. Safe to call mid-synth — the in-flight
	 * call still holds a TSharedPtr to the runner and finishes cleanly,
	 * after which the runner's destructor runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void UnloadModel();

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	bool IsModelLoaded() const;

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	EInoNeuTtsVariant GetCurrentVariant() const { return CurrentVariant; }

	/**
	 * Audio output sample rate in Hz. Baked into NeuCodec at 24,000;
	 * the same value for every voice and both Nano / Air variants.
	 * Use this to configure UStreamingSoundWave::SetSampleRate before
	 * feeding chunks from OnAudioChunk.
	 */
	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	int32 GetSampleRate() const { return 24000; }

	/**
	 * Audio output channel count. NeuTTS always emits mono (1 channel).
	 * Use this with UStreamingSoundWave::SetNumOfChannels.
	 */
	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	int32 GetNumChannels() const { return 1; }

	// ---- Synthesis ----

	/**
	 * Synthesize Text into 24 kHz mono int16 PCM in the given voice.
	 * Dispatches to the UE thread pool. Fires OnComplete on the game
	 * thread when done. Errors immediately if a previous synth is
	 * still in flight (one at a time in v1).
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void SynthesizeAsync(
		const FString& Text,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		const FInoNeuTtsSynthesisCompleteDelegate& OnComplete);

	/**
	 * Streaming variant of SynthesizeAsync. Same end-state (OnComplete
	 * fires with the full waveform), but as the worker generates the
	 * AR loop it emits overlap-added chunks of `ChunkTokens` codec
	 * frames each via OnAudioChunk. Set ChunkTokens to 0 to use the
	 * default of 25 (matches Neuphonic's reference; ~0.5 s per chunk
	 * at 24 kHz). The final chunk has bIsFinal=true; OnComplete fires
	 * immediately afterwards.
	 *
	 * Errors immediately if a previous synth (one-shot or streaming) is
	 * still in flight.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void SynthesizeStreamAsync(
		const FString& Text,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		int32 ChunkTokens,
		const FInoNeuTtsAudioChunkDelegate& OnAudioChunk,
		const FInoNeuTtsSynthesisCompleteDelegate& OnComplete);

	/** Cooperative abort of the in-flight synth. Fires OnComplete with bSuccess=false. */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void CancelSynthesis();

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	bool IsSynthInFlight() const { return bSynthInFlight; }

	// ---- Voice loading ----

	/**
	 * Load a single .nvoice.json into a voice struct. Returns false
	 * with OutVoice's bIsValid=false on any error.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	bool LoadVoiceFromFile(const FString& FilePath, FInoNeuTtsVoice& OutVoice);

	/**
	 * Scan Plugins/InoAgents/NeuTTS/voices/ and return all parsed
	 * voices. Cheap (only the JSON parses run; no model state).
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	TArray<FInoNeuTtsVoice> ListBundledVoices();

private:
	/** Game-thread-only: dispatched delegate after model load. */
	void HandleModelLoaded(
		TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> NewRunner,
		EInoNeuTtsVariant Variant,
		FString Error,
		FInoNeuTtsLoadedDelegate Delegate);

	/** Game-thread-only: dispatched delegate after synth completion. */
	void HandleSynthComplete(
		FInoNeuTtsResult Result,
		FInoNeuTtsSynthesisCompleteDelegate Delegate);

	/**
	 * The loaded engine state. TSharedPtr (thread-safe) so an in-flight
	 * synth on a worker thread keeps it alive across UnloadModel.
	 */
	TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> Runner;

	/**
	 * Cancel flag for the current/pending synth. Reset at synth start,
	 * set by CancelSynthesis. The worker checks via raw pointer; the
	 * shared_ptr keeps it alive as long as the worker holds its copy.
	 */
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CurrentCancelFlag;

	EInoNeuTtsVariant CurrentVariant = EInoNeuTtsVariant::Nano;

	/** Game-thread-only flags. */
	bool bIsLoading      = false;
	bool bSynthInFlight  = false;
};
