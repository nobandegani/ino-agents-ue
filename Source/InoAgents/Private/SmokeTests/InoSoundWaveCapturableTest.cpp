// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.SoundWave.Capturable* smoke tests
// ============================================================================
//
//   Ino.SoundWave.ListDevices
//     Queries available audio input devices via the static
//     UInoCapturableSoundWave::GetAvailableAudioInputDevices helper
//     and logs each one (index, name, sample rate, channels, AEC).
//
//   Ino.SoundWave.Capture [durationSec=2]
//     Opens the default input device, captures audio for the given
//     duration (default 2 s), then stops and reports how many samples
//     arrived via OnPopulateAudioData. Passes if at least one populate
//     fire happened (confirming mic → wave round trip works).
// ============================================================================

#include "InoSoundWaveCapturableTest.h"

#include "Audio/InoCapturableSoundWave.h"
#include "InoAgentsLog.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

// ---------------------------------------------------------------------------
// Observer method impls
// ---------------------------------------------------------------------------

void UInoSoundWaveCaptureTestObserver::HandleStarted()
{
    bCaptureStarted = true;
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Log, TEXT("CaptureTest: OnCaptureStarted (+%.3f s)"), Elapsed);
}

void UInoSoundWaveCaptureTestObserver::HandleStopped()
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Log,
           TEXT("CaptureTest: OnCaptureStopped (+%.3f s). PopulateFires=%d, TotalSamples=%lld"),
           Elapsed, PopulateFires, static_cast<long long>(TotalSamplesSeen));

    if (bCaptureStarted && PopulateFires > 0)
    {
        UE_LOG(LogInoAgents, Log, TEXT("CaptureTest: PASS"));
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("CaptureTest: FAIL — bCaptureStarted=%s, PopulateFires=%d"),
               bCaptureStarted ? TEXT("true") : TEXT("false"), PopulateFires);
    }

    Wave = nullptr;
    RemoveFromRoot();
}

void UInoSoundWaveCaptureTestObserver::HandlePopulate(const TArray<float>& Data)
{
    ++PopulateFires;
    TotalSamplesSeen += Data.Num();
    if (PopulateFires == 1)
    {
        const double Elapsed = FPlatformTime::Seconds() - StartTime;
        UE_LOG(LogInoAgents, Log,
               TEXT("CaptureTest: first OnPopulateAudioData (+%.3f s, %d samples)"),
               Elapsed, Data.Num());
    }
}

void UInoSoundWaveCaptureTestObserver::HandleError(FString ErrorMessage)
{
    UE_LOG(LogInoAgents, Error, TEXT("CaptureTest: error: %s"), *ErrorMessage);
}

// ---------------------------------------------------------------------------
// ListDevices command
// ---------------------------------------------------------------------------

static void RunListDevicesTest(const TArray<FString>& /*Args*/)
{
    UE_LOG(LogInoAgents, Log, TEXT("ListDevices: querying available audio input devices..."));

    FOnInoGetAvailableAudioInputDevicesResultNative Result;
    Result.BindLambda(
        [](const TArray<FInoAudioInputDeviceInfo>& Devices)
        {
            UE_LOG(LogInoAgents, Log, TEXT("ListDevices: %d device(s):"), Devices.Num());
            for (int32 i = 0; i < Devices.Num(); ++i)
            {
                const FInoAudioInputDeviceInfo& D = Devices[i];
                UE_LOG(LogInoAgents, Log,
                       TEXT("  [%d] %s (id=%s, %d Hz, %d ch, hwAEC=%s)"),
                       i, *D.DeviceName, *D.DeviceId,
                       D.PreferredSampleRate, D.InputChannels,
                       D.bSupportsHardwareAEC ? TEXT("yes") : TEXT("no"));
            }
            if (Devices.Num() == 0)
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("ListDevices: no devices found — check OS input permissions"));
            }
        });
    UInoCapturableSoundWave::GetAvailableAudioInputDevices(Result);
}

static FAutoConsoleCommand GListDevicesCmd(
    TEXT("Ino.SoundWave.ListDevices"),
    TEXT("Enumerate available audio input devices and log each one. "
         "Useful to find a device index to pass to the Capture command."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunListDevicesTest));

// ---------------------------------------------------------------------------
// Capture command
// ---------------------------------------------------------------------------

static void RunCaptureTest(const TArray<FString>& Args)
{
    const float DurationSec = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2.0f;
    const float ClampedSec  = FMath::Clamp(DurationSec, 0.25f, 30.0f);

    UInoCapturableSoundWave* Wave =
        UInoCapturableSoundWave::CreateCapturableSoundWave();
    if (Wave == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("CaptureTest: CreateCapturableSoundWave returned null"));
        return;
    }

    UInoSoundWaveCaptureTestObserver* Observer =
        NewObject<UInoSoundWaveCaptureTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Wave      = Wave;
    Observer->AddToRoot();

    Wave->OnCaptureStarted.AddDynamic(
        Observer, &UInoSoundWaveCaptureTestObserver::HandleStarted);
    Wave->OnCaptureStopped.AddDynamic(
        Observer, &UInoSoundWaveCaptureTestObserver::HandleStopped);
    Wave->OnPopulateAudioData.AddDynamic(
        Observer, &UInoSoundWaveCaptureTestObserver::HandlePopulate);
    Wave->OnAudioError.AddDynamic(
        Observer, &UInoSoundWaveCaptureTestObserver::HandleError);

    // Default device = -1 (system default input).
    if (!Wave->StartCapture(-1))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("CaptureTest: StartCapture(-1) failed — no default input "
                    "device, or audio capture not supported on this platform."));
        Observer->Wave = nullptr;
        Observer->RemoveFromRoot();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("CaptureTest: capturing for %.2f s..."), ClampedSec);

    // Schedule a stop via FTSTicker — AddTicker returns immediately and
    // the stop happens on the next game thread tick after DurationSec.
    TWeakObjectPtr<UInoSoundWaveCaptureTestObserver> WeakObs(Observer);
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [WeakObs](float) -> bool
            {
                if (UInoSoundWaveCaptureTestObserver* Obs = WeakObs.Get())
                {
                    if (Obs->Wave != nullptr)
                    {
                        Obs->Wave->StopCapture();
                    }
                }
                return false;
            }),
        ClampedSec);
}

static FAutoConsoleCommand GCaptureCmd(
    TEXT("Ino.SoundWave.Capture"),
    TEXT("Open the default audio input device, capture for N seconds "
         "(default 2), then stop. Passes if at least one OnPopulateAudioData "
         "fire happens. Usage: Ino.SoundWave.Capture [seconds]"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunCaptureTest));
