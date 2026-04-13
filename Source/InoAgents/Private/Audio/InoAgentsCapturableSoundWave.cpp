// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsCapturableSoundWave.h"

#include "InoAgentsLog.h"

#include "Async/Async.h"

// Capture is currently wired only on Windows and Mac (the two
// platforms where AudioCaptureRtAudio is available per the
// Build.cs gate). On other platforms the class still compiles and
// all UFUNCTIONs exist — they just fail gracefully, so existing
// Blueprint graphs don't break when targeting mobile.
#if PLATFORM_WINDOWS || PLATFORM_MAC
    #include "AudioCaptureCore.h"
    #define WITH_INOAGENTS_CAPTURE 1
#else
    #define WITH_INOAGENTS_CAPTURE 0
#endif

// ---------------------------------------------------------------------------
// Opaque capture state (pimpl)
// ---------------------------------------------------------------------------
//
// Holding Audio::FAudioCapture via TPimplPtr keeps AudioCaptureCore.h out
// of the public header. On unsupported platforms the struct is empty and
// the pimpl still compiles; the capture methods short-circuit via
// WITH_INOAGENTS_CAPTURE before touching it.
class FInoAgentsAudioCaptureState
{
public:
#if WITH_INOAGENTS_CAPTURE
    Audio::FAudioCapture Capture;
#endif
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

UInoAgentsCapturableSoundWave::UInoAgentsCapturableSoundWave(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    CaptureState = MakePimpl<FInoAgentsAudioCaptureState>();
}

void UInoAgentsCapturableSoundWave::BeginDestroy()
{
    // Flip the capture gate before touching FAudioCapture — the
    // OnCapture lambda checks this bool and bails if false, so any
    // in-flight callback returns immediately without touching us.
    bIsCapturing.Store(false);

#if WITH_INOAGENTS_CAPTURE
    if (CaptureState.IsValid())
    {
        // Abort forcibly instead of a clean Stop() — BeginDestroy is
        // on the critical teardown path and we don't want to block
        // waiting for the capture thread to land a clean frame.
        if (CaptureState->Capture.IsStreamOpen())
        {
            CaptureState->Capture.AbortStream();
            CaptureState->Capture.CloseStream();
        }
    }
#endif
    Super::BeginDestroy();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UInoAgentsCapturableSoundWave* UInoAgentsCapturableSoundWave::CreateCapturableSoundWave()
{
    return NewObject<UInoAgentsCapturableSoundWave>();
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

void UInoAgentsCapturableSoundWave::GetAvailableAudioInputDevices(
    const FOnInoAgentsGetAvailableAudioInputDevicesResult& Result)
{
    FOnInoAgentsGetAvailableAudioInputDevicesResultNative Native;
    Native.BindLambda(
        [Result](const TArray<FInoAgentsAudioInputDeviceInfo>& Devices)
        {
            Result.ExecuteIfBound(Devices);
        });
    GetAvailableAudioInputDevices(Native);
}

void UInoAgentsCapturableSoundWave::GetAvailableAudioInputDevices(
    const FOnInoAgentsGetAvailableAudioInputDevicesResultNative& Result)
{
#if WITH_INOAGENTS_CAPTURE
    // Enumeration is cheap on Windows / Mac (ms-scale) but the API
    // returns synchronously, so we hop to the thread pool and back
    // to keep the signature async-friendly. Frees the game thread
    // during what might otherwise be an unexpected stall on systems
    // with many devices.
    Async(EAsyncExecution::ThreadPool, [Result]()
    {
        Audio::FAudioCapture Capture;
        TArray<Audio::FCaptureDeviceInfo> Raw;
        const int32 NumFound = Capture.GetCaptureDevicesAvailable(Raw);

        TArray<FInoAgentsAudioInputDeviceInfo> Out;
        Out.Reserve(Raw.Num());
        for (int32 i = 0; i < Raw.Num(); ++i)
        {
            FInoAgentsAudioInputDeviceInfo Info;
            Info.DeviceName = Raw[i].DeviceName;
            // The engine's FCaptureDeviceInfo.DeviceId is an opaque
            // platform string (e.g. a GUID on Windows). We surface it
            // verbatim; StartCapture uses the array index rather than
            // parsing the ID, so the UI layer doesn't need to care
            // about its format.
            Info.DeviceId             = Raw[i].DeviceId.IsEmpty()
                                        ? FString::FromInt(i)
                                        : Raw[i].DeviceId;
            Info.InputChannels        = Raw[i].InputChannels;
            Info.PreferredSampleRate  = Raw[i].PreferredSampleRate;
            Info.bSupportsHardwareAEC = Raw[i].bSupportsHardwareAEC;
            Out.Add(MoveTemp(Info));
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsCapturableSoundWave: found %d input device(s)"),
               NumFound);

        AsyncTask(ENamedThreads::GameThread,
            [Result, Out = MoveTemp(Out)]()
            {
                Result.ExecuteIfBound(Out);
            });
    });
#else
    // No capture backend on this platform — return an empty list
    // on the next tick so callers don't get a re-entrant callback.
    AsyncTask(ENamedThreads::GameThread, [Result]()
    {
        Result.ExecuteIfBound(TArray<FInoAgentsAudioInputDeviceInfo>());
    });
#endif
}

// ---------------------------------------------------------------------------
// Capture lifecycle
// ---------------------------------------------------------------------------

bool UInoAgentsCapturableSoundWave::StartCapture(int32 DeviceId)
{
#if WITH_INOAGENTS_CAPTURE
    if (!CaptureState.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsCapturableSoundWave::StartCapture: capture state invalid"));
        return false;
    }
    if (bIsCapturing.Load())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoAgentsCapturableSoundWave::StartCapture: already capturing"));
        return false;
    }

    // Probe the requested device for its preferred rate / channels —
    // trying to open with an unsupported (rate, channels) pair returns
    // false at OpenAudioCaptureStream.
    Audio::FCaptureDeviceInfo Info;
    const bool bGotInfo = CaptureState->Capture.GetCaptureDeviceInfo(Info, DeviceId);
    if (!bGotInfo)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsCapturableSoundWave::StartCapture: no info for device %d"),
               DeviceId);
        return false;
    }

    // Configure the streaming wave's format up front so the first
    // AppendAudioDataFromRAW call from the capture thread doesn't
    // reject us. SetInitialDesiredSampleRate / SetInitialDesiredNumChannels
    // are safe to call before any data lands.
    SetInitialDesiredSampleRate(Info.PreferredSampleRate);
    SetInitialDesiredNumChannels(Info.InputChannels);

    // Bind a capture callback that shuttles frames straight into the
    // streaming append path. The callback fires on the platform capture
    // thread (e.g. WASAPI worker on Windows) — AppendAudioDataFromRAW
    // serialises through the wave's task pipe so we can safely call
    // from any thread.
    TWeakObjectPtr<UInoAgentsCapturableSoundWave> WeakThis(this);
    Audio::FOnAudioCaptureFunction OnCapture =
        [WeakThis](const void* InAudio, int32 NumFrames, int32 NumChannels,
                   int32 SampleRate, double /*StreamTime*/, bool /*bOverflow*/)
        {
            UInoAgentsCapturableSoundWave* Self = WeakThis.Get();
            if (Self == nullptr || !Self->bIsCapturing.Load()
                || InAudio == nullptr || NumFrames <= 0 || NumChannels <= 0)
            {
                return;
            }

            // Default encoding is FLOATING_POINT_32 so InAudio is a
            // float*. We wrap it as uint8 bytes and let
            // AppendAudioDataFromRAW transcode (the transcoder for
            // Float32 is just clamp + copy).
            const int32 NumBytes =
                NumFrames * NumChannels * static_cast<int32>(sizeof(float));
            TArray<uint8> Bytes;
            Bytes.SetNumUninitialized(NumBytes);
            if (Self->bMuted.Load())
            {
                // Zero-fill so muting shows up as silence downstream
                // rather than a stalled visualization feed.
                FMemory::Memset(Bytes.GetData(), 0, NumBytes);
            }
            else
            {
                FMemory::Memcpy(Bytes.GetData(), InAudio, NumBytes);
            }

            Self->AppendAudioDataFromRAW(
                Bytes,
                EInoAgentsRAWAudioFormat::Float32,
                SampleRate,
                NumChannels);
        };

    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex       = DeviceId;
    Params.NumInputChannels  = Info.InputChannels;
    Params.SampleRate        = Info.PreferredSampleRate;
    Params.PCMAudioEncoding  = Audio::DefaultDeviceEncoding;  // FLOATING_POINT_32
    Params.bUseHardwareAEC   = false;

    // NumFramesDesired controls the OS buffer size — 1024 gives about
    // 21 ms at 48 kHz, which is a decent trade between latency and
    // callback overhead.
    const uint32 NumFramesDesired = 1024;
    const bool bOpened = CaptureState->Capture.OpenAudioCaptureStream(
        Params, MoveTemp(OnCapture), NumFramesDesired);
    if (!bOpened)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsCapturableSoundWave::StartCapture: OpenAudioCaptureStream failed"));
        return false;
    }

    if (!CaptureState->Capture.StartStream())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsCapturableSoundWave::StartCapture: StartStream failed"));
        CaptureState->Capture.CloseStream();
        return false;
    }

    bIsCapturing.Store(true);
    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsCapturableSoundWave::StartCapture: device %d, %d Hz / %d ch"),
           DeviceId, Info.PreferredSampleRate, Info.InputChannels);

    OnCaptureStartedNative.Broadcast();
    OnCaptureStarted.Broadcast();
    return true;
#else
    UE_LOG(LogInoAgents, Error,
           TEXT("UInoAgentsCapturableSoundWave::StartCapture: "
                "audio capture not supported on this platform"));
    return false;
#endif
}

void UInoAgentsCapturableSoundWave::StopCapture()
{
#if WITH_INOAGENTS_CAPTURE
    if (!bIsCapturing.Load() || !CaptureState.IsValid())
    {
        return;
    }

    bIsCapturing.Store(false);

    if (CaptureState->Capture.IsCapturing())
    {
        CaptureState->Capture.StopStream();
    }
    if (CaptureState->Capture.IsStreamOpen())
    {
        CaptureState->Capture.CloseStream();
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsCapturableSoundWave::StopCapture: stream closed"));

    OnCaptureStoppedNative.Broadcast();
    OnCaptureStopped.Broadcast();
#endif
}

bool UInoAgentsCapturableSoundWave::ToggleMute(bool bMute)
{
    bMuted.Store(bMute);
    return bMute;
}

bool UInoAgentsCapturableSoundWave::IsCapturing() const
{
#if WITH_INOAGENTS_CAPTURE
    return bIsCapturing.Load()
        && CaptureState.IsValid()
        && CaptureState->Capture.IsCapturing();
#else
    return false;
#endif
}
