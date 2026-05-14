// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTTS/InoNeuTTSSubsystem.h"

#include "InoNeuTTSRunner.h"
#include "InoNeuTTSSettings.h"
#include "InoNeuTTSSynthesisWorker.h"
#include "InoNeuTTSVoiceAsset.h"

#include "InoAgentsLog.h"
#include "InoDownloader.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

void UInoNeuTTSSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    // Members are zero-initialized; nothing to do beyond this. Engine +
    // decoder loads happen lazily in LoadModelAsync.
}

void UInoNeuTTSSubsystem::Deinitialize()
{
    UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][Subsystem] Deinitialize — tearing down."));

    // Signal in-flight synth to abort. The worker checks this flag at
    // phase boundaries and bails out; we then drop our Runner reference.
    if (CurrentCancelFlag.IsValid())
    {
        CurrentCancelFlag->store(true, std::memory_order_release);
    }

    // Cancel in-flight download. InoNodes deletes its .partial file
    // and fires OnComplete(bSuccess=false / "Cancelled"); our weak-this
    // check there will see the subsystem is gone and exit cleanly.
    if (DownloadCancelToken.IsValid())
    {
        DownloadCancelToken->Cancel();
    }

    Runner.Reset();
    ActiveVoice = FInoNeuTTSVoice();
    ActiveVoiceName.Reset();

    Super::Deinitialize();
}

// =====================================================================
//  Model lifecycle
// =====================================================================

void UInoNeuTTSSubsystem::LoadModelAsync(
    const FInoNeuTTSConfig& Config,
    const FInoNeuTTSLoadedDelegate& OnLoaded,
    const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress)
{
    DownloadEntriesInternal(Config, /*bDownloadOnly=*/false,
                            OnLoaded, OnDownloadProgress);
}

void UInoNeuTTSSubsystem::DownloadModelAsync(
    const FInoNeuTTSConfig& Config,
    const FInoNeuTTSLoadedDelegate& OnComplete,
    const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress)
{
    DownloadEntriesInternal(Config, /*bDownloadOnly=*/true,
                            OnComplete, OnDownloadProgress);
}

void UInoNeuTTSSubsystem::DownloadEntriesInternal(
    const FInoNeuTTSConfig& Config,
    bool bDownloadOnly,
    const FInoNeuTTSLoadedDelegate& OnComplete,
    const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress)
{
    check(IsInGameThread());

    if (bIsLoading)
    {
        OnComplete.ExecuteIfBound(false, TEXT("Another load is already in flight"));
        return;
    }
    if (!bDownloadOnly && Runner.IsValid())
    {
        OnComplete.ExecuteIfBound(false,
            TEXT("A model is already loaded — call UnloadModel first"));
        return;
    }

    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings)
    {
        OnComplete.ExecuteIfBound(false, TEXT("UInoNeuTTSSettings::Get() returned NULL"));
        return;
    }
    const FInoNeuTTSBackboneEntry* Backbone = Settings->FindBackbone(Config.BackboneModelName);
    const FInoNeuTTSDecoderEntry*  Decoder  = Settings->FindDecoder(Config.DecoderModelName);
    if (!Backbone)
    {
        OnComplete.ExecuteIfBound(false, FString::Printf(
            TEXT("No backbone entry matches '%s' in Project Settings → Plugins → InoNeuTTS"),
            *Config.BackboneModelName));
        return;
    }
    if (!Decoder)
    {
        OnComplete.ExecuteIfBound(false, FString::Printf(
            TEXT("No decoder entry matches '%s' in Project Settings → Plugins → InoNeuTTS"),
            *Config.DecoderModelName));
        return;
    }

    // Build batch of 2 download requests (backbone first, decoder second).
    TArray<FInoDownloadRequest> Reqs;
    {
        FInoDownloadRequest R;
        R.Url                = Backbone->DownloadUrl;
        R.SaveDirectory      = UInoNeuTTSSettings::GetModelsDir();
        R.FileName           = Backbone->LocalFileName;
        R.ExpectedSha256     = Backbone->ExpectedSha256;
        R.ExpectedTotalBytes = Backbone->FileSizeBytes;
        R.bSkipIfCached      = true;
        Reqs.Add(R);
    }
    {
        FInoDownloadRequest R;
        R.Url                = Decoder->DownloadUrl;
        R.SaveDirectory      = UInoNeuTTSSettings::GetModelsDir();
        R.FileName           = Decoder->LocalFileName;
        R.ExpectedSha256     = Decoder->ExpectedSha256;
        R.ExpectedTotalBytes = Decoder->FileSizeBytes;
        R.bSkipIfCached      = true;
        Reqs.Add(R);
    }

    // Early-out for entries with empty URL + missing cached file.
    for (const FInoDownloadRequest& R : Reqs)
    {
        const FString TargetPath = FPaths::Combine(R.SaveDirectory, R.FileName);
        if (R.Url.IsEmpty() && !IFileManager::Get().FileExists(*TargetPath))
        {
            OnComplete.ExecuteIfBound(false, FString::Printf(
                TEXT("File '%s' not found on disk and no DownloadUrl configured. ")
                TEXT("Drop it at %s or set DownloadUrl in Project Settings → Plugins → InoNeuTTS."),
                *R.FileName, *TargetPath));
            return;
        }
    }

    // Stash state.
    bIsLoading                = true;
    bPendingIsDownloadOnly    = bDownloadOnly;
    LoadedConfig              = Config;
    PendingOnLoaded           = OnComplete;
    PendingOnDownloadProgress = OnDownloadProgress;
    DownloadCancelToken       = MakeShared<FInoCancellationToken, ESPMode::ThreadSafe>();

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Subsystem] %s starting (backbone=%s, decoder=%s)"),
        bDownloadOnly ? TEXT("DownloadModelAsync") : TEXT("LoadModelAsync"),
        *Backbone->LocalFileName, *Decoder->LocalFileName);

    TWeakObjectPtr<UInoNeuTTSSubsystem> WeakThis(this);
    InoNodes::Download::DownloadFilesAsync(
        Reqs,
        // Progress — already game-thread by InoNodes contract.
        [WeakThis](const FInoDownloadProgress& P)
        {
            if (UInoNeuTTSSubsystem* Self = WeakThis.Get())
            {
                Self->PendingOnDownloadProgress.ExecuteIfBound(P);
            }
        },
        // Completion — game thread.
        [WeakThis](const TArray<FInoDownloadResult>& Results)
        {
            UInoNeuTTSSubsystem* Self = WeakThis.Get();
            if (!Self) return;  // subsystem gone — InoNodes already cleaned up
            Self->DownloadCancelToken.Reset();

            for (const FInoDownloadResult& R : Results)
            {
                if (!R.bSuccess)
                {
                    Self->FinishLoad(false, FString::Printf(
                        TEXT("Download failed for %s: %s"), *R.FileName, *R.ErrorMessage));
                    return;
                }
            }

            // Both files are on disk + (if SHA configured) verified.
            if (Self->bPendingIsDownloadOnly)
            {
                Self->FinishLoad(true, FString());
                return;
            }
            // Hand off to ThreadPool for runner creation.
            const FString BackbonePath = Results[0].AbsolutePath;
            const FString DecoderPath  = Results[1].AbsolutePath;
            Self->DispatchRunnerLoad(BackbonePath, DecoderPath);
        },
        DownloadCancelToken);
}

void UInoNeuTTSSubsystem::DispatchRunnerLoad(
    const FString& BackbonePath, const FString& DecoderPath)
{
    check(IsInGameThread());

    TWeakObjectPtr<UInoNeuTTSSubsystem> WeakThis(this);
    const FString BackbonePathFull = FPaths::ConvertRelativePathToFull(BackbonePath);
    const FString DecoderPathFull  = FPaths::ConvertRelativePathToFull(DecoderPath);
    // Snapshot the entire LoadedConfig so the worker has its own copy —
    // Config carries all backend / activation / cache-dir / warmup knobs.
    const FInoNeuTTSConfig ConfigCopy = LoadedConfig;

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Subsystem] dispatching runner load (backbone=%s, decoder=%s, ")
        TEXT("backbone_backend=%d, decoder_backend=%d, activation=%d, max_tokens=%d, ")
        TEXT("warmup_backbone=%d, warmup_decoder=%d)"),
        *BackbonePathFull, *DecoderPathFull,
        static_cast<int32>(ConfigCopy.BackboneBackend),
        static_cast<int32>(ConfigCopy.DecoderBackend),
        static_cast<int32>(ConfigCopy.ActivationType),
        ConfigCopy.MaxNumTokens,
        ConfigCopy.bWarmupBackboneOnLoad ? 1 : 0,
        ConfigCopy.bWarmupDecoderOnLoad ? 1 : 0);

    Async(EAsyncExecution::ThreadPool,
        [BackbonePathFull, DecoderPathFull, ConfigCopy, WeakThis]()
    {
        // ============= WORKER THREAD =============
        // Do not touch WeakThis here — weak-pointer access is game-thread only.
        FString Err;
        TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> NewRunner =
            FInoNeuTTSRunner::Create(BackbonePathFull, DecoderPathFull,
                                     ConfigCopy, Err);

        // Marshal back to game thread to commit the runner.
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, NewRunner, Err]() mutable
        {
            UInoNeuTTSSubsystem* Self = WeakThis.Get();
            if (!Self) return;

            if (!NewRunner.IsValid())
            {
                Self->FinishLoad(false, FString::Printf(
                    TEXT("Runner creation failed: %s"), *Err));
                return;
            }
            Self->Runner = NewRunner;
            Self->FinishLoad(true, FString());
        });
    });
}

void UInoNeuTTSSubsystem::FinishLoad(bool bSuccess, const FString& ErrorMessage)
{
    check(IsInGameThread());

    bIsLoading = false;
    bPendingIsDownloadOnly = false;
    // Snapshot + clear pending delegates before firing so a re-entrant
    // LoadModelAsync from inside the handler sees clean state.
    FInoNeuTTSLoadedDelegate Cb = PendingOnLoaded;
    PendingOnLoaded            = FInoNeuTTSLoadedDelegate();
    PendingOnDownloadProgress  = FInoNeuTTSDownloadProgressDelegate();

    if (bSuccess)
    {
        UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][Subsystem] load OK."));
    }
    else
    {
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Subsystem] load FAILED: %s"), *ErrorMessage);
        // Drop any partially-populated runner on failure.
        Runner.Reset();
    }
    Cb.ExecuteIfBound(bSuccess, ErrorMessage);
}

bool UInoNeuTTSSubsystem::IsModelLoaded() const
{
    return Runner.IsValid();
}

bool UInoNeuTTSSubsystem::IsModelDownloaded(const FInoNeuTTSConfig& Config) const
{
    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings) return false;
    const FInoNeuTTSBackboneEntry* Backbone = Settings->FindBackbone(Config.BackboneModelName);
    const FInoNeuTTSDecoderEntry*  Decoder  = Settings->FindDecoder(Config.DecoderModelName);
    if (!Backbone || !Decoder) return false;

    const FString BackbonePath = UInoNeuTTSSettings::ResolveLocalPath(Backbone->LocalFileName);
    const FString DecoderPath  = UInoNeuTTSSettings::ResolveLocalPath(Decoder->LocalFileName);

    IFileManager& FM = IFileManager::Get();
    return FM.FileExists(*BackbonePath) && FM.FileSize(*BackbonePath) > 0
        && FM.FileExists(*DecoderPath)  && FM.FileSize(*DecoderPath)  > 0;
}

void UInoNeuTTSSubsystem::UnloadModel()
{
    check(IsInGameThread());
    if (CurrentCancelFlag.IsValid())
    {
        CurrentCancelFlag->store(true, std::memory_order_release);
    }
    Runner.Reset();
    ActiveVoice = FInoNeuTTSVoice();
    ActiveVoiceName.Reset();
    UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][Subsystem] UnloadModel — runner dropped."));
}

// =====================================================================
//  Active voice
// =====================================================================

void UInoNeuTTSSubsystem::SetActiveVoiceAsync(
    UInoNeuTTSVoiceAsset* VoiceAsset,
    const FInoNeuTTSVoiceReadyDelegate& OnReady)
{
    check(IsInGameThread());

    if (!Runner.IsValid())
    {
        OnReady.ExecuteIfBound(false, TEXT("Model not loaded"));
        return;
    }
    if (!VoiceAsset || !VoiceAsset->IsUsable())
    {
        OnReady.ExecuteIfBound(false,
            TEXT("Voice asset is null or not usable (missing Name / Language / RefCodes)"));
        return;
    }
    if (bIsPrimingVoice)
    {
        OnReady.ExecuteIfBound(false, TEXT("Voice priming already in flight"));
        return;
    }
    if (bSynthInFlight)
    {
        // Priming mutates the runner's cache; running concurrently with a
        // synth that's reading the cache would race. Cancel first or wait.
        OnReady.ExecuteIfBound(false,
            TEXT("Cannot change active voice while a synth is in flight — call CancelSynthesis first"));
        return;
    }

    const FInoNeuTTSVoice Voice = VoiceAsset->ToRuntimeVoice();
    bIsPrimingVoice = true;

    TWeakObjectPtr<UInoNeuTTSSubsystem> WeakThis(this);
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;

    Async(EAsyncExecution::ThreadPool,
        [RunnerCopy, Voice, WeakThis, OnReady]()
    {
        FString Err;
        const bool bOK = RunnerCopy->PrimeVoice(Voice, Err);

        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, bOK, Err, Voice, OnReady]()
        {
            if (UInoNeuTTSSubsystem* Self = WeakThis.Get())
            {
                Self->bIsPrimingVoice = false;
                if (bOK)
                {
                    Self->ActiveVoice     = Voice;
                    Self->ActiveVoiceName = Voice.Name;
                }
            }
            OnReady.ExecuteIfBound(bOK, Err);
        });
    });
}

void UInoNeuTTSSubsystem::ClearActiveVoice()
{
    check(IsInGameThread());
    if (Runner.IsValid()) Runner->ClearVoiceCache();
    ActiveVoice = FInoNeuTTSVoice();
    ActiveVoiceName.Reset();
}

// =====================================================================
//  Synthesis
// =====================================================================

void UInoNeuTTSSubsystem::SynthesizeAsync(
    const FString& Text,
    const FInoNeuTTSOptions& Options,
    const FInoNeuTTSSynthesisCompleteDelegate& OnComplete)
{
    check(IsInGameThread());

    auto FailWith = [&](const FString& Err)
    {
        FInoNeuTTSResult R;
        R.ErrorMessage = Err;
        OnComplete.ExecuteIfBound(R);
    };

    if (!Runner.IsValid())          { FailWith(TEXT("Model not loaded")); return; }
    if (!ActiveVoice.bIsValid)      { FailWith(TEXT("No active voice set — call SetActiveVoiceAsync first")); return; }
    if (bIsPrimingVoice)            { FailWith(TEXT("Voice priming in flight — wait for OnReady before synthesizing")); return; }
    if (bSynthInFlight)             { FailWith(TEXT("Another synth is already in flight")); return; }

    bSynthInFlight    = true;
    CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

    TWeakObjectPtr<UInoNeuTTSSubsystem> WeakThis(this);
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
    FInoNeuTTSVoice VoiceCopy = ActiveVoice;
    auto CancelCopy = CurrentCancelFlag;

    Async(EAsyncExecution::ThreadPool,
        [RunnerCopy, Text, VoiceCopy, Options, CancelCopy, WeakThis, OnComplete]()
    {
        FInoNeuTTSResult Result = InoNeuTTSNative::RunSynthesis(
            RunnerCopy.Get(), Text, VoiceCopy, Options, CancelCopy);

        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Result = MoveTemp(Result), OnComplete]() mutable
        {
            if (UInoNeuTTSSubsystem* Self = WeakThis.Get())
            {
                Self->bSynthInFlight = false;
                Self->CurrentCancelFlag.Reset();
            }
            OnComplete.ExecuteIfBound(Result);
        });
    });
}

void UInoNeuTTSSubsystem::SynthesizeStreamAsync(
    const FString& Text,
    const FInoNeuTTSOptions& Options,
    int32 ChunkTokens,
    const FInoNeuTTSAudioChunkDelegate& OnAudioChunk,
    const FInoNeuTTSSynthesisCompleteDelegate& OnComplete)
{
    check(IsInGameThread());

    auto FailWith = [&](const FString& Err)
    {
        FInoNeuTTSResult R;
        R.ErrorMessage = Err;
        OnComplete.ExecuteIfBound(R);
    };

    if (!Runner.IsValid())          { FailWith(TEXT("Model not loaded")); return; }
    if (!ActiveVoice.bIsValid)      { FailWith(TEXT("No active voice set")); return; }
    if (bIsPrimingVoice)            { FailWith(TEXT("Voice priming in flight — wait for OnReady before synthesizing")); return; }
    if (bSynthInFlight)             { FailWith(TEXT("Another synth is already in flight")); return; }

    bSynthInFlight    = true;
    CurrentCancelFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

    TWeakObjectPtr<UInoNeuTTSSubsystem> WeakThis(this);
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> RunnerCopy = Runner;
    FInoNeuTTSVoice VoiceCopy = ActiveVoice;
    auto CancelCopy = CurrentCancelFlag;

    Async(EAsyncExecution::ThreadPool,
        [RunnerCopy, Text, VoiceCopy, Options, ChunkTokens, CancelCopy, WeakThis,
         OnAudioChunk, OnComplete]()
    {
        // Bounce chunk callbacks onto the game thread.
        InoNeuTTSNative::FStreamChunkFn ChunkBridge =
            [WeakThis, OnAudioChunk](const TArray<uint8>& AudioChunk, bool bIsFinal)
        {
            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, AudioChunk, bIsFinal, OnAudioChunk]()
            {
                if (WeakThis.IsValid())
                {
                    OnAudioChunk.ExecuteIfBound(AudioChunk, bIsFinal);
                }
            });
        };

        FInoNeuTTSResult Result = InoNeuTTSNative::RunStreamingSynthesis(
            RunnerCopy.Get(), Text, VoiceCopy, Options, ChunkTokens,
            ChunkBridge, CancelCopy);

        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Result = MoveTemp(Result), OnComplete]() mutable
        {
            if (UInoNeuTTSSubsystem* Self = WeakThis.Get())
            {
                Self->bSynthInFlight = false;
                Self->CurrentCancelFlag.Reset();
            }
            OnComplete.ExecuteIfBound(Result);
        });
    });
}

void UInoNeuTTSSubsystem::CancelSynthesis()
{
    check(IsInGameThread());
    if (CurrentCancelFlag.IsValid())
    {
        CurrentCancelFlag->store(true, std::memory_order_release);
        UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][Subsystem] CancelSynthesis — flag set."));
    }
}
