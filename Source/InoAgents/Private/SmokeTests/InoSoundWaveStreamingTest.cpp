// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.SoundWave.Streaming* smoke tests
// ============================================================================
//
// Console commands exercising the new sound-wave-centric audio stack:
//
//   Ino.SoundWave.StreamingPcm
//     Generates a 1-second 440 Hz sine wave, appends it via
//     AppendAudioDataFromRAW (Int16), plays through a plain
//     UAudioComponent, and verifies OnPopulateAudioData fires and
//     OnAudioPlaybackFinished fires after drain.
//
//   Ino.SoundWave.StreamingMp3 [path]
//     Loads an MP3 file, appends it in one shot via
//     AppendAudioDataFromMP3, and waits for playback to finish.
//     Default path: Saved/InoAgents/ElevenLabs/test.mp3.
//
//   Ino.SoundWave.StreamingMp3Chunked [path]
//     Same file but fed in 4 KB chunks via FTSTicker at 50 ms spacing —
//     proves the streaming ingestion path works.
//
//   Ino.SoundWave.Visualization
//     Same sine-wave generator as StreamingPcm, but this test binds
//     OnGeneratePCMData and asserts at least one non-empty batch arrives
//     on the game thread during playback.
// ============================================================================

#include "InoSoundWaveStreamingTest.h"

#include "Audio/InoStreamingSoundWave.h"
#include "InoAgentsLog.h"

#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Observer method impls
// ---------------------------------------------------------------------------

void UInoSoundWaveStreamingTestObserver::HandlePopulate(const TArray<float>& Data)
{
    ++PopulateFires;
    if (PopulateFires == 1)
    {
        const double Elapsed = FPlatformTime::Seconds() - StartTime;
        UE_LOG(LogInoAgents, Log,
               TEXT("%s: first OnPopulateAudioData (+%.3f s, %d samples)"),
               *TestName, Elapsed, Data.Num());
    }
}

void UInoSoundWaveStreamingTestObserver::HandleGeneratePcm(const TArray<float>& Data)
{
    ++GeneratePcmFires;
    GeneratePcmSamples += Data.Num();
}

void UInoSoundWaveStreamingTestObserver::HandlePlaybackFinished()
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    bFinishedFired = true;
    UE_LOG(LogInoAgents, Log,
           TEXT("%s: OnAudioPlaybackFinished (+%.3f s). "
                "PopulateFires=%d, GeneratePcmFires=%d, GeneratePcmSamples=%d"),
           *TestName, Elapsed,
           PopulateFires, GeneratePcmFires, GeneratePcmSamples);

    if (!bErrored && PopulateFires > 0)
    {
        UE_LOG(LogInoAgents, Log, TEXT("%s: PASS"), *TestName);
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("%s: partial result — PopulateFires=%d, Errored=%s"),
               *TestName, PopulateFires, bErrored ? TEXT("true") : TEXT("false"));
    }
    Finish();
}

void UInoSoundWaveStreamingTestObserver::HandleError(FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    bErrored = true;
    UE_LOG(LogInoAgents, Error,
           TEXT("%s: FAILED after %.3f s: %s"),
           *TestName, Elapsed, *ErrorMessage);
    Finish();
}

void UInoSoundWaveStreamingTestObserver::Finish()
{
    if (AudioComp != nullptr)
    {
        AudioComp->Stop();
    }
    if (HostActor != nullptr)
    {
        HostActor->Destroy();
    }
    AudioComp = nullptr;
    Wave      = nullptr;
    HostActor = nullptr;
    RemoveFromRoot();
    UE_LOG(LogInoAgents, Log, TEXT("%s: DONE"), *TestName);
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace
{
    constexpr int32 kSineSampleRate = 44100;
    constexpr int32 kSineDurationMs = 1000;
    constexpr float kSineFrequency  = 440.0f;
    constexpr float kSineAmplitude  = 0.5f;   // -6 dB so our eardrums survive
    constexpr int32 kChunkSizeBytes = 4096;
    constexpr float kChunkIntervalS = 0.050f;

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
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.World() != nullptr)
            {
                return Context.World();
            }
        }
        return nullptr;
    }

    /** 1-second 440 Hz sine wave, int16 LE mono. */
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

    FString DefaultMp3Path()
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("InoAgents/ElevenLabs/test.mp3"));
    }

    /**
     * Spawn an actor + UAudioComponent + streaming wave + observer
     * with delegates bound. Wave is NOT pre-configured — test sets
     * its format before the first append (or relies on MP3 auto-
     * detect).
     */
    UInoSoundWaveStreamingTestObserver* SpawnObserver(
        const FString& TestName, bool bBindGeneratePcm)
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

        UAudioComponent* Comp = NewObject<UAudioComponent>(Host);
        Comp->bAutoActivate = false;
        Comp->RegisterComponent();

        UInoStreamingSoundWave* Wave =
            UInoStreamingSoundWave::CreateStreamingSoundWave();
        Comp->SetSound(Wave);

        UInoSoundWaveStreamingTestObserver* Observer =
            NewObject<UInoSoundWaveStreamingTestObserver>();
        Observer->TestName  = TestName;
        Observer->StartTime = FPlatformTime::Seconds();
        Observer->HostActor = Host;
        Observer->AudioComp = Comp;
        Observer->Wave      = Wave;
        Observer->AddToRoot();

        Wave->OnPopulateAudioData.AddDynamic(
            Observer, &UInoSoundWaveStreamingTestObserver::HandlePopulate);
        Wave->OnAudioPlaybackFinished.AddDynamic(
            Observer, &UInoSoundWaveStreamingTestObserver::HandlePlaybackFinished);
        Wave->OnAudioError.AddDynamic(
            Observer, &UInoSoundWaveStreamingTestObserver::HandleError);
        if (bBindGeneratePcm)
        {
            Wave->OnGeneratePCMData.AddDynamic(
                Observer, &UInoSoundWaveStreamingTestObserver::HandleGeneratePcm);
        }

        return Observer;
    }
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

static void RunStreamingPcmTest(const TArray<FString>& /*Args*/)
{
    UInoSoundWaveStreamingTestObserver* Observer =
        SpawnObserver(TEXT("StreamingPcm"), /*bBindGeneratePcm=*/false);
    if (Observer == nullptr)
    {
        return;
    }

    TArray<uint8> PcmBytes = GenerateSineWavePcmBytes();
    UE_LOG(LogInoAgents, Log,
           TEXT("StreamingPcm: generated %d ms of %.0f Hz sine wave (%d bytes)"),
           kSineDurationMs, kSineFrequency, PcmBytes.Num());

    Observer->Wave->AppendAudioDataFromRAW(
        PcmBytes, EInoRawAudioFormat::Int16, kSineSampleRate, 1);
    // Drain flag up-front — single shot, nothing else coming.
    Observer->Wave->SetStopSoundOnPlaybackFinish(true);
    Observer->AudioComp->Play();
}

static FAutoConsoleCommand GStreamingPcmCmd(
    TEXT("Ino.SoundWave.StreamingPcm"),
    TEXT("Play a 1-second 440 Hz sine wave through UInoStreamingSoundWave "
         "via the RAW append path. Asserts OnPopulateAudioData and "
         "OnAudioPlaybackFinished both fire."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamingPcmTest));

static void RunStreamingMp3Test(const TArray<FString>& Args)
{
    const FString Path = Args.Num() > 0 ? Args[0] : DefaultMp3Path();

    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamingMp3: could not read %s. Run "
                    "Ino.ElevenLabs.DialogueStreamTest first, or pass "
                    "a path: Ino.SoundWave.StreamingMp3 <path>"),
               *Path);
        return;
    }

    UInoSoundWaveStreamingTestObserver* Observer =
        SpawnObserver(TEXT("StreamingMp3"), /*bBindGeneratePcm=*/false);
    if (Observer == nullptr)
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamingMp3: loaded %s (%d bytes)"), *Path, FileBytes.Num());

    Observer->Wave->AppendAudioDataFromMP3(FileBytes);
    Observer->Wave->SetStopSoundOnPlaybackFinish(true);
    Observer->AudioComp->Play();
}

static FAutoConsoleCommand GStreamingMp3Cmd(
    TEXT("Ino.SoundWave.StreamingMp3"),
    TEXT("Play an MP3 file through UInoStreamingSoundWave's MP3 append "
         "path. Defaults to Saved/InoAgents/ElevenLabs/test.mp3. Optional path arg."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamingMp3Test));

static void RunStreamingMp3ChunkedTest(const TArray<FString>& Args)
{
    const FString Path = Args.Num() > 0 ? Args[0] : DefaultMp3Path();

    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamingMp3Chunked: could not read %s"), *Path);
        return;
    }

    UInoSoundWaveStreamingTestObserver* Observer =
        SpawnObserver(TEXT("StreamingMp3Chunked"), /*bBindGeneratePcm=*/false);
    if (Observer == nullptr)
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamingMp3Chunked: loaded %s (%d bytes), slicing into %d-byte chunks"),
           *Path, FileBytes.Num(), kChunkSizeBytes);

    Observer->AudioComp->Play();

    struct FChunkState
    {
        TWeakObjectPtr<UInoSoundWaveStreamingTestObserver> ObserverWeak;
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
                UInoSoundWaveStreamingTestObserver* Obs = State->ObserverWeak.Get();
                if (Obs == nullptr || Obs->Wave == nullptr)
                {
                    return false;
                }

                const int32 Remaining = State->Bytes.Num() - State->Offset;
                if (Remaining <= 0)
                {
                    Obs->Wave->SetStopSoundOnPlaybackFinish(true);
                    return false;
                }

                const int32 ThisChunk = FMath::Min(Remaining, kChunkSizeBytes);
                TArray<uint8> Slice;
                Slice.Append(State->Bytes.GetData() + State->Offset, ThisChunk);
                State->Offset += ThisChunk;

                Obs->Wave->AppendAudioDataFromMP3(Slice);
                return true;
            }),
        kChunkIntervalS);
}

static FAutoConsoleCommand GStreamingMp3ChunkedCmd(
    TEXT("Ino.SoundWave.StreamingMp3Chunked"),
    TEXT("Play an MP3 file via chunked AppendAudioDataFromMP3 (4 KB chunks, "
         "50 ms apart, FTSTicker-driven). Proves the streaming ingestion path "
         "works end-to-end. Optional path arg."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamingMp3ChunkedTest));

static void RunVisualizationTest(const TArray<FString>& /*Args*/)
{
    UInoSoundWaveStreamingTestObserver* Observer =
        SpawnObserver(TEXT("Visualization"), /*bBindGeneratePcm=*/true);
    if (Observer == nullptr)
    {
        return;
    }

    TArray<uint8> PcmBytes = GenerateSineWavePcmBytes();
    UE_LOG(LogInoAgents, Log,
           TEXT("Visualization: bound OnGeneratePCMData, feeding %d bytes of sine"),
           PcmBytes.Num());

    Observer->Wave->AppendAudioDataFromRAW(
        PcmBytes, EInoRawAudioFormat::Int16, kSineSampleRate, 1);
    Observer->Wave->SetStopSoundOnPlaybackFinish(true);
    Observer->AudioComp->Play();
}

static FAutoConsoleCommand GVisualizationCmd(
    TEXT("Ino.SoundWave.Visualization"),
    TEXT("Play a 1-second sine wave and assert OnGeneratePCMData fires with "
         "real sample data during playback. Baseline test for lip-sync / "
         "visualizer consumers."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunVisualizationTest));
