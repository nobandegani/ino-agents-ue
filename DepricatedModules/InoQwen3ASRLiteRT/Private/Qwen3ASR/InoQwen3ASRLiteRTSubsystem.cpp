// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRLiteRTSubsystem.h"

#include "InoQwen3ASRLiteRT.h"
#include "InoQwen3ASRRunner.h"
#include "InoQwen3ASRTranscriptionWorker.h"
#include "InoQwen3ASRWavLoader.h"

#include "Async/Async.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

UInoQwen3ASRLiteRTSubsystem::UInoQwen3ASRLiteRTSubsystem() = default;
UInoQwen3ASRLiteRTSubsystem::UInoQwen3ASRLiteRTSubsystem(FVTableHelper& Helper)
    : Super(Helper)
{
}
UInoQwen3ASRLiteRTSubsystem::~UInoQwen3ASRLiteRTSubsystem() = default;

void UInoQwen3ASRLiteRTSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogInoQwen3ASRLiteRT, Verbose,
        TEXT("UInoQwen3ASRLiteRTSubsystem::Initialize"));
}

void UInoQwen3ASRLiteRTSubsystem::Deinitialize()
{
    // Tear the worker down BEFORE the runner — the worker's thread holds a
    // raw pointer into the runner and might still be processing a request.
    // Destroying the worker joins its thread, guaranteeing nothing is
    // running by the time we drop the runner.
    Worker.Reset();
    Runner.Reset();
    bModelLoaded = false;
    bModelLoading = false;
    UE_LOG(LogInoQwen3ASRLiteRT, Verbose,
        TEXT("UInoQwen3ASRLiteRTSubsystem::Deinitialize"));
    Super::Deinitialize();
}

FString UInoQwen3ASRLiteRTSubsystem::ResolveModelPath(const FString& FileName)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid()) { return FString(); }
    return FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("models"), FileName);
}

FString UInoQwen3ASRLiteRTSubsystem::ResolveVocabPath(const FString& FileName)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid()) { return FString(); }
    return FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("tokenizer"), FileName);
}

void UInoQwen3ASRLiteRTSubsystem::LoadModelAsync(
    const FInoQwen3ASRConfig& Config,
    const FOnInoQwen3ASRModelLoaded& OnLoaded)
{
    if (bModelLoading)
    {
        OnLoaded.ExecuteIfBound(false, TEXT("Already loading"));
        return;
    }
    if (bModelLoaded)
    {
        OnLoaded.ExecuteIfBound(true, FString());
        return;
    }

    const FString ModelPath = ResolveModelPath(Config.ModelFileName);
    const FString VocabPath = ResolveVocabPath(Config.VocabFileName);
    if (ModelPath.IsEmpty() || !FPaths::FileExists(ModelPath))
    {
        OnLoaded.ExecuteIfBound(false,
            FString::Printf(TEXT("Model file not found: %s"), *ModelPath));
        return;
    }
    if (VocabPath.IsEmpty() || !FPaths::FileExists(VocabPath))
    {
        OnLoaded.ExecuteIfBound(false,
            FString::Printf(TEXT("Vocab file not found: %s"), *VocabPath));
        return;
    }

    bModelLoading = true;
    TWeakObjectPtr<UInoQwen3ASRLiteRTSubsystem> WeakThis(this);

    Async(EAsyncExecution::ThreadPool,
        [WeakThis, ModelPath, VocabPath, OnLoaded]()
        {
            // ============== background thread: load model + tokenizer ==============
            TUniquePtr<FInoQwen3ASRRunner> NewRunner = MakeUnique<FInoQwen3ASRRunner>();

            FString FailReason;
            const double T0 = FPlatformTime::Seconds();
            const bool bModelOk = NewRunner->LoadModel(ModelPath);
            if (!bModelOk)
            {
                FailReason = FString::Printf(
                    TEXT("FInoQwen3ASRRunner::LoadModel failed for '%s'."), *ModelPath);
            }
            const bool bTokOk = bModelOk && NewRunner->LoadTokenizer(VocabPath);
            if (bModelOk && !bTokOk)
            {
                FailReason = FString::Printf(
                    TEXT("FInoQwen3ASRRunner::LoadTokenizer failed for '%s'."), *VocabPath);
            }
            const double Elapsed = FPlatformTime::Seconds() - T0;

            // =================== marshal back to game thread ====================
            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, NewRunner = MoveTemp(NewRunner), bModelOk, bTokOk, FailReason, Elapsed, OnLoaded]() mutable
                {
                    if (!WeakThis.IsValid())
                    {
                        // Subsystem went away while we were loading. Drop the
                        // runner silently — its destructor releases the
                        // model + KV buffers safely from any thread.
                        return;
                    }
                    UInoQwen3ASRLiteRTSubsystem* Self = WeakThis.Get();
                    Self->bModelLoading = false;
                    if (bModelOk && bTokOk)
                    {
                        Self->Runner = MoveTemp(NewRunner);
                        // Worker takes a raw pointer to the runner; both live
                        // until Deinitialize tears them down in the right order.
                        Self->Worker = MakeUnique<FInoQwen3ASRTranscriptionWorker>(Self->Runner.Get());
                        Self->bModelLoaded = true;
                        UE_LOG(LogInoQwen3ASRLiteRT, Log,
                            TEXT("Subsystem: model+tokenizer loaded in %.2fs."), Elapsed);
                        OnLoaded.ExecuteIfBound(true, FString());
                    }
                    else
                    {
                        UE_LOG(LogInoQwen3ASRLiteRT, Error,
                            TEXT("Subsystem: model load failed — %s"), *FailReason);
                        OnLoaded.ExecuteIfBound(false, FailReason);
                    }
                });
        });
}

void UInoQwen3ASRLiteRTSubsystem::TranscribeAudioAsync(
    const TArray<float>& AudioSamples,
    const FOnInoQwen3ASRTranscribeComplete& OnComplete)
{
    if (!bModelLoaded || !Worker)
    {
        OnComplete.ExecuteIfBound(false, FInoQwen3ASRTranscribeResult{},
            TEXT("Model not loaded — call LoadModelAsync first."));
        return;
    }
    if (AudioSamples.Num() == 0)
    {
        OnComplete.ExecuteIfBound(false, FInoQwen3ASRTranscribeResult{},
            TEXT("Empty audio buffer."));
        return;
    }

    // Owner = this subsystem; if we're GC'd before the result is dispatched
    // (Deinitialize already nulled Worker so this is mostly defensive), the
    // dispatch is silently skipped.
    TArray<float> AudioCopy = AudioSamples;
    Worker->EnqueueRequest(MoveTemp(AudioCopy), TWeakObjectPtr<UObject>(this), OnComplete);
}

void UInoQwen3ASRLiteRTSubsystem::TranscribeWavFileAsync(
    const FString& WavPath,
    const FOnInoQwen3ASRTranscribeComplete& OnComplete)
{
    if (!bModelLoaded || !Worker)
    {
        OnComplete.ExecuteIfBound(false, FInoQwen3ASRTranscribeResult{},
            TEXT("Model not loaded — call LoadModelAsync first."));
        return;
    }

    // Load the WAV synchronously on the game thread. The file is small
    // (5 s of int16 audio = 160 KB) and the parse is microseconds.
    // Off-loading this would just add complexity for no gain.
    TArray<float> Samples;
    if (!InoQwen3ASR::LoadWav16kMonoFromDisk(WavPath, Samples))
    {
        OnComplete.ExecuteIfBound(false, FInoQwen3ASRTranscribeResult{},
            FString::Printf(TEXT("WAV load failed: '%s' (must be 16 kHz mono PCM int16/float32)."),
                            *WavPath));
        return;
    }
    Worker->EnqueueRequest(MoveTemp(Samples), TWeakObjectPtr<UObject>(this), OnComplete);
}

void UInoQwen3ASRLiteRTSubsystem::CancelTranscription()
{
    if (Worker)
    {
        Worker->CancelPending();
    }
}

int32 UInoQwen3ASRLiteRTSubsystem::GetQueueDepth() const
{
    return Worker ? Worker->GetQueueDepthApprox() : 0;
}
