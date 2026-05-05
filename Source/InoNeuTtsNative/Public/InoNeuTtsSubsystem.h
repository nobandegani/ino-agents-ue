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
 *   1. LoadModelAsync(Config, OnLoaded, OnDownloadProgress)  // off-thread,
 *                                                             // fires on game thread
 *   2. ListBundledVoices() / LoadVoiceFromFile  // build / pick a voice
 *   3. SetActiveVoiceAsync(Voice, OnReady)      // primes voice cache
 *   4. SynthesizeAsync(Text, Options, OnComplete)  // (any number of times)
 *   5. (optionally CancelSynthesis() to abort an in-flight call)
 *   6. UnloadModel()                            // releases GGUF + ONNX
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
	 * Load the GGUF backbone + ONNX decoder for the configured backbone
	 * entry. No-op (with a warning) if the same model is already loaded;
	 * call UnloadModel first to switch.
	 *
	 * Dispatches downloading + the multi-second model mmap to a thread
	 * pool worker so the game thread stays responsive throughout.
	 *
	 *   OnLoaded             — fires once on the game thread when load
	 *                          either finishes (bSuccess=true) or fails
	 *                          (bSuccess=false + ErrorMessage).
	 *   OnDownloadProgress   — fires zero or more times on the game
	 *                          thread during the download phase. Skipped
	 *                          entirely when both files are already
	 *                          cached on disk. Carries the same shared
	 *                          FInoDownloadProgress struct InoNodes-driven
	 *                          downloads use everywhere else (per-file +
	 *                          overall %, BytesPerSecond + ETA, current
	 *                          file name + index, retry attempt).
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void LoadModelAsync(
		const FInoNeuTtsConfig& Config,
		const FInoNeuTtsLoadedDelegate& OnLoaded,
		const FInoNeuTtsDownloadProgressDelegate& OnDownloadProgress);

	/**
	 * Drop the loaded model. Safe to call mid-synth — the in-flight
	 * call still holds a TSharedPtr to the runner and finishes cleanly,
	 * after which the runner's destructor runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void UnloadModel();

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	bool IsModelLoaded() const;

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

	// ---- Active voice (KV-cache prefix priming) ----

	/**
	 * Pre-cache a voice for synthesis. Tokenizes + prefills the fixed
	 * prompt prefix once and snapshots the post-prefix KV state, so
	 * every subsequent voice-less SynthesizeAsync call skips the prefix
	 * prefill (~5–10% of total synth time).
	 *
	 * Dispatches the prefill + snapshot to a thread pool worker
	 * (200–600 ms depending on the model). Fires OnReady on the game
	 * thread when done.
	 *
	 * After OnReady fires with bSuccess=true:
	 *   - The voice-less SynthesizeAsync / SynthesizeStreamAsync
	 *     overloads will use this voice automatically.
	 *   - The per-voice overloads still work; if you pass the same
	 *     voice, they hit the cache; if you pass a different one,
	 *     they fall back to a full prefill (the cache stays primed
	 *     for the next call with the matching voice).
	 *
	 * Errors immediately if no model is loaded or a previous priming /
	 * synth is still in flight.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void SetActiveVoiceAsync(
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsVoiceReadyDelegate& OnReady);

	/**
	 * Drop the cached active voice. Subsequent voice-less SynthesizeAsync
	 * calls will fail until a new voice is set. Cheap; no LM state changes.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void ClearActiveVoice();

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	bool HasActiveVoice() const;

	UFUNCTION(BlueprintPure, Category = "InoNeuTts")
	FString GetActiveVoiceName() const { return ActiveVoiceName; }

	// ---- Synthesis ----

	/**
	 * Synthesize Text into 24 kHz mono int16 PCM using the active voice
	 * (set via SetActiveVoiceAsync). Dispatches to the UE thread pool;
	 * fires OnComplete on the game thread.
	 *
	 * Errors immediately if no active voice is set, no model is loaded,
	 * Text is empty, or a previous synth is still in flight.
	 *
	 * The runner uses the active voice's KV-prefix snapshot to skip the
	 * prefix prefill on every synth — that's the whole point of the
	 * SetActiveVoice flow. To switch voice, call SetActiveVoiceAsync
	 * with the new one and wait for OnReady before calling this again.
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void SynthesizeAsync(
		const FString& Text,
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
	 * Same precondition + error rules as SynthesizeAsync (active voice
	 * required; one synth at a time).
	 */
	UFUNCTION(BlueprintCallable, Category = "InoNeuTts")
	void SynthesizeStreamAsync(
		const FString& Text,
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
	/**
	 * State for an in-flight LoadModelAsync — survives the download +
	 * model-load chain via TSharedRef capture in the helper methods
	 * below. Lifetime: created at LoadModelAsync's start, destroyed when
	 * FinishLoadJob fires on the game thread.
	 */
	struct FLoadJob
	{
		FInoNeuTtsConfig Config;
		FString GgufPath;
		FString OnnxPath;
		FString BackboneName;          // for diagnostics + cache logging
		FInoNeuTtsLoadedDelegate           OnLoaded;
		FInoNeuTtsDownloadProgressDelegate OnDownloadProgress;
	};

	/** Sequential async chain. Each step calls the next on success or FinishLoadJob on error. */
	void EnsureFilesDownloaded(TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job);
	void DispatchModelLoad   (TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job);
	void FinishLoadJob       (TSharedRef<FLoadJob, ESPMode::ThreadSafe> Job, bool bSuccess, FString Error);

	/** Game-thread-only: dispatched delegate after model load. */
	void HandleModelLoaded(
		TSharedPtr<InoNeuTtsNative::FInoNeuTtsRunner, ESPMode::ThreadSafe> NewRunner,
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

	/**
	 * DisplayName of the backbone currently loaded (or empty). Used to
	 * skip a redundant LoadModelAsync that targets the same backbone.
	 */
	FString CurrentBackboneName;

	/**
	 * Cached copy of the active voice (the one whose KV-prefix snapshot
	 * lives on the runner). Stored so the voice-less SynthesizeAsync
	 * overloads can pass it back into the underlying per-voice synth
	 * pipeline.
	 */
	FInoNeuTtsVoice ActiveVoice;

	/**
	 * Display name of the active voice ("" when nothing is primed).
	 * Cheap to read from Blueprint without copying the whole voice.
	 */
	FString ActiveVoiceName;

	/** Game-thread-only flags. */
	bool bIsLoading      = false;
	bool bIsPrimingVoice = false;
	bool bSynthInFlight  = false;
};
