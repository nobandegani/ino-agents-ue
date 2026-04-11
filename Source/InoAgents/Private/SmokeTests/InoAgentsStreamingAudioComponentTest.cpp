// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.Audio.* streaming audio component smoke tests
// ============================================================================
//
// Three console commands exercise the full surface of
// UInoAgentsStreamingAudioComponent end-to-end inside PIE:
//
//   InoAgents.Audio.PlayPcmTest
//     Generates a 1-second 440 Hz sine wave (44100 Hz int16 mono),
//     feeds it in one shot via PlayAudio(bytes, PcmInt16), and verifies
//     OnReadyToPlay + OnFinished fire in order. No decoder path
//     involved — the PCM bytes go straight into USoundWaveProcedural.
//
//   InoAgents.Audio.PlayMp3Test [path]
//     Reads an MP3 file from disk and plays it in one shot via
//     PlayAudio(bytes, Mp3). Defaults to
//     Saved/InoAgents/ElevenLabs/test.mp3 (what the ElevenLabs
//     smoke test writes), so you can run ElevenLabs' smoke test
//     first and then this to hear what ElevenLabs just generated.
//     Override the path by passing it as the console arg.
//
//   InoAgents.Audio.PlayMp3ChunkedTest [path]
//     Same file as PlayMp3Test but fed in 4 KB chunks spaced 50 ms
//     apart via FTSTicker, then FinalizeStream(). Proves the chunked
//     ingestion path works and playback starts before all chunks
//     arrive.
//
// Each test spawns a disposable AActor in the PIE world, attaches
// a UInoAgentsStreamingAudioComponent, and tears down on the
// terminal delegate. No assets, no actors in the level, no
// persistent state.
// ============================================================================

#include "InoAgentsStreamingAudioComponentTest.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "InoAgentsLog.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    constexpr int32 kSineSampleRate = 44100;
    constexpr int32 kSineDurationMs = 1000;
    constexpr float kSineFrequency  = 440.0f;
    constexpr float kSineAmplitude  = 0.5f;   // -6 dB so our eardrums survive
    constexpr int32 kChunkSizeBytes = 4096;
    constexpr float kChunkIntervalS = 0.050f; // 50 ms

    /** Find a live PIE world so we can spawn the host actor. */
    UWorld* FindPieWorld()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.World() != nullptr && Context.WorldType == EWorldType::PIE)
            {
                return Context.World();
            }
        }
        // Fallback: any world (editor preview, auto-tests). Better
        // than bailing.
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.World() != nullptr)
            {
                return Context.World();
            }
        }
        return nullptr;
    }

    /** Build a 1-second 440 Hz sine wave as int16 LE mono bytes. */
    TArray<uint8> GenerateSineWavePcmBytes()
    {
        const int32 NumSamples = (kSineSampleRate * kSineDurationMs) / 1000;
        TArray<uint8> Bytes;
        Bytes.Reserve(NumSamples * sizeof(int16));

        const double TwoPi = 6.28318530717958647692;
        for (int32 i = 0; i < NumSamples; ++i)
        {
            const double t      = static_cast<double>(i) / static_cast<double>(kSineSampleRate);
            const double Sample = FMath::Sin(TwoPi * kSineFrequency * t);
            const int16  Value  = static_cast<int16>(Sample * kSineAmplitude * 32767.0);
            Bytes.Add(static_cast<uint8>(Value & 0xFF));
            Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
        }
        return Bytes;
    }

    /** Default MP3 location — whatever the ElevenLabs smoke test writes. */
    FString DefaultMp3Path()
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("InoAgents/ElevenLabs/test.mp3"));
    }

    /** Spawn a transient actor + component pair and bind delegates. */
    UInoAgentsStreamingAudioComponentTestObserver* SpawnObserver(
        const FString& TestName)
    {
        UWorld* World = FindPieWorld();
        if (World == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("%s: no world available — start PIE first."), *TestName);
            return nullptr;
        }

        AActor* Host = World->SpawnActor<AActor>();
        if (Host == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("%s: SpawnActor failed"), *TestName);
            return nullptr;
        }

        UInoAgentsStreamingAudioComponent* Comp =
            NewObject<UInoAgentsStreamingAudioComponent>(Host);
        Comp->RegisterComponent();

        UInoAgentsStreamingAudioComponentTestObserver* Observer =
            NewObject<UInoAgentsStreamingAudioComponentTestObserver>();
        Observer->TestName  = TestName;
        Observer->StartTime = FPlatformTime::Seconds();
        Observer->HostActor = Host;
        Observer->AudioComp = Comp;
        Observer->AddToRoot();

        Comp->OnReadyToPlay.AddDynamic(
            Observer, &UInoAgentsStreamingAudioComponentTestObserver::HandleReadyToPlay);
        Comp->OnFinished.AddDynamic(
            Observer, &UInoAgentsStreamingAudioComponentTestObserver::HandleFinished);
        Comp->OnError.AddDynamic(
            Observer, &UInoAgentsStreamingAudioComponentTestObserver::HandleError);

        return Observer;
    }
}

// ---------------------------------------------------------------------------
// Observer handlers
// ---------------------------------------------------------------------------

void UInoAgentsStreamingAudioComponentTestObserver::HandleReadyToPlay()
{
    ReadyTime   = FPlatformTime::Seconds();
    bReadyFired = true;
    UE_LOG(LogInoAgents, Log,
           TEXT("%s: OnReadyToPlay (+%.3f s)"),
           *TestName, ReadyTime - StartTime);
}

void UInoAgentsStreamingAudioComponentTestObserver::HandleFinished()
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    bFinishedFired = true;
    UE_LOG(LogInoAgents, Log,
           TEXT("%s: OnFinished (+%.3f s total)"), *TestName, Elapsed);

    if (bReadyFired && !bErrored)
    {
        UE_LOG(LogInoAgents, Log, TEXT("%s: PASS"), *TestName);
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("%s: partial result — Ready=%s, Errored=%s"),
               *TestName,
               bReadyFired ? TEXT("true") : TEXT("false"),
               bErrored    ? TEXT("true") : TEXT("false"));
    }

    Finish();
}

void UInoAgentsStreamingAudioComponentTestObserver::HandleError(FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    bErrored = true;
    UE_LOG(LogInoAgents, Error,
           TEXT("%s: FAILED after %.3f s: %s"),
           *TestName, Elapsed, *ErrorMessage);
    Finish();
}

void UInoAgentsStreamingAudioComponentTestObserver::Finish()
{
    if (AudioComp != nullptr)
    {
        AudioComp->StopAndReset();
    }
    if (HostActor != nullptr)
    {
        HostActor->Destroy();
    }
    AudioComp = nullptr;
    HostActor = nullptr;
    RemoveFromRoot();
    UE_LOG(LogInoAgents, Log, TEXT("%s: DONE"), *TestName);
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

static void RunPlayPcmTest(const TArray<FString>& /*Args*/)
{
    UInoAgentsStreamingAudioComponentTestObserver* Observer =
        SpawnObserver(TEXT("PlayPcmTest"));
    if (Observer == nullptr)
    {
        return;
    }

    TArray<uint8> PcmBytes = GenerateSineWavePcmBytes();
    UE_LOG(LogInoAgents, Log,
           TEXT("PlayPcmTest: generated %d ms of %.0f Hz sine wave (%d bytes)"),
           kSineDurationMs, kSineFrequency, PcmBytes.Num());

    Observer->AudioComp->SetPcmFormat(kSineSampleRate, /*NumChannels=*/1);
    Observer->AudioComp->PlayAudio(PcmBytes, EInoAgentsAudioFormat::PcmInt16);
}

static FAutoConsoleCommand GPlayPcmTestCommand(
    TEXT("InoAgents.Audio.PlayPcmTest"),
    TEXT("Streaming audio smoke test (PCM path): generates a 1-second 440 Hz "
         "sine wave at 44100 Hz int16 mono, plays it via "
         "UInoAgentsStreamingAudioComponent::PlayAudio, asserts OnReadyToPlay "
         "and OnFinished both fire."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunPlayPcmTest));

static void RunPlayMp3Test(const TArray<FString>& Args)
{
    const FString Path = Args.Num() > 0 ? Args[0] : DefaultMp3Path();

    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("PlayMp3Test: could not read %s. Run "
                    "InoAgents.ElevenLabs.DialogueStreamTest first to produce one, "
                    "or pass a path: InoAgents.Audio.PlayMp3Test <path>"),
               *Path);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("PlayMp3Test: loaded %s (%d bytes)"), *Path, FileBytes.Num());

    UInoAgentsStreamingAudioComponentTestObserver* Observer =
        SpawnObserver(TEXT("PlayMp3Test"));
    if (Observer == nullptr)
    {
        return;
    }

    Observer->AudioComp->PlayAudio(FileBytes, EInoAgentsAudioFormat::Mp3);
}

static FAutoConsoleCommand GPlayMp3TestCommand(
    TEXT("InoAgents.Audio.PlayMp3Test"),
    TEXT("Streaming audio smoke test (MP3 path, one-shot): loads an MP3 file "
         "from disk and plays it via UInoAgentsStreamingAudioComponent::PlayAudio. "
         "Defaults to Saved/InoAgents/ElevenLabs/test.mp3 (written by "
         "InoAgents.ElevenLabs.DialogueStreamTest). Optional path arg."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunPlayMp3Test));

static void RunPlayMp3ChunkedTest(const TArray<FString>& Args)
{
    const FString Path = Args.Num() > 0 ? Args[0] : DefaultMp3Path();

    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("PlayMp3ChunkedTest: could not read %s. Run "
                    "InoAgents.ElevenLabs.DialogueStreamTest first to produce one, "
                    "or pass a path: InoAgents.Audio.PlayMp3ChunkedTest <path>"),
               *Path);
        return;
    }

    UInoAgentsStreamingAudioComponentTestObserver* Observer =
        SpawnObserver(TEXT("PlayMp3ChunkedTest"));
    if (Observer == nullptr)
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("PlayMp3ChunkedTest: loaded %s (%d bytes), slicing into %d-byte chunks"),
           *Path, FileBytes.Num(), kChunkSizeBytes);

    // Capture state by shared pointer so the ticker can safely mutate
    // an offset index across firings. The observer's UPROPERTY owns
    // the component for lifetime.
    struct FChunkState
    {
        TWeakObjectPtr<UInoAgentsStreamingAudioComponentTestObserver> ObserverWeak;
        TArray<uint8> Bytes;
        int32         Offset = 0;
    };
    TSharedRef<FChunkState> State = MakeShared<FChunkState>();
    State->ObserverWeak = Observer;
    State->Bytes        = MoveTemp(FileBytes);

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [State](float /*DeltaTime*/) -> bool
            {
                UInoAgentsStreamingAudioComponentTestObserver* Obs = State->ObserverWeak.Get();
                if (Obs == nullptr || Obs->AudioComp == nullptr)
                {
                    return false;  // stop ticking
                }

                const int32 Remaining = State->Bytes.Num() - State->Offset;
                if (Remaining <= 0)
                {
                    Obs->AudioComp->FinalizeStream();
                    return false;  // stop ticking
                }

                const int32 ThisChunk = FMath::Min(Remaining, kChunkSizeBytes);
                TArray<uint8> Slice;
                Slice.Append(State->Bytes.GetData() + State->Offset, ThisChunk);
                State->Offset += ThisChunk;

                Obs->AudioComp->FeedAudioBytes(Slice, EInoAgentsAudioFormat::Mp3);
                return true;  // keep ticking
            }),
        kChunkIntervalS);
}

static FAutoConsoleCommand GPlayMp3ChunkedTestCommand(
    TEXT("InoAgents.Audio.PlayMp3ChunkedTest"),
    TEXT("Streaming audio smoke test (MP3 path, chunked): loads an MP3 file, "
         "slices it into 4 KB chunks, feeds them one per 50 ms via "
         "FTSTicker + FeedAudioBytes, then FinalizeStream. Proves playback "
         "starts before all chunks are delivered."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunPlayMp3ChunkedTest));
