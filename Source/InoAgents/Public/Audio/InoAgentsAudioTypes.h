// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoAgentsAudioTypes.generated.h"

// =====================================================================
// RAW PCM sample encoding
// =====================================================================

/**
 * How each sample in a RAW PCM byte buffer is encoded.
 *
 * Only describes the per-sample numeric type. Sample rate and channel
 * count are separate parameters on AppendAudioDataFromRAW — RAW PCM
 * bytes don't carry that metadata.
 *
 * Obsolete / pro-audio-only formats (int8, int24, uint16, uint32) are
 * not exposed. Convert to Int16 or Float32 on your side if you need
 * them.
 */
UENUM(BlueprintType)
enum class EInoAgentsRAWAudioFormat : uint8
{
    /** Signed 16-bit integer, little-endian, interleaved.
     *  Universal standard for TTS / voice streams. 2 bytes per sample
     *  per channel. */
    Int16       UMETA(DisplayName = "Int16 (signed 16-bit)"),

    /** Signed 32-bit integer, little-endian, interleaved. 4 bytes per
     *  sample per channel. Uncommon — most pipelines use Int16 or
     *  Float32. */
    Int32       UMETA(DisplayName = "Int32 (signed 32-bit)"),

    /** Unsigned 8-bit, centered at 128, interleaved. 1 byte per sample
     *  per channel. Low-bandwidth / legacy format. */
    UInt8       UMETA(DisplayName = "UInt8 (unsigned 8-bit)"),

    /** IEEE 754 single-precision float, interleaved. Samples in
     *  [-1.0, +1.0]; out-of-range clamped. 4 bytes per sample per
     *  channel. Native format for USoundWaveProcedural buffers. */
    Float32     UMETA(DisplayName = "Float32 (-1.0 to +1.0)"),
};

// =====================================================================
// Audio input device info (capture)
// =====================================================================

/**
 * One entry in the list of available audio-input devices.
 * Populated by UInoAgentsCapturableSoundWave::GetAvailableAudioInputDevices.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoAgentsAudioInputDeviceInfo
{
    GENERATED_BODY()

    /** Human-readable device name shown in system audio settings. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Audio")
    FString DeviceName;

    /** Opaque platform device identifier. Pass this to StartCapture. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Audio")
    FString DeviceId;

    /** Native channel count reported by the OS (1 = mono, 2 = stereo). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Audio")
    int32 InputChannels = 0;

    /** Device's preferred sample rate (Hz). 0 = unknown. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Audio")
    int32 PreferredSampleRate = 0;

    /** True if the device reports hardware acoustic-echo cancellation
     *  is enabled. Informational only — we don't toggle it. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Audio")
    bool bSupportsHardwareAEC = false;
};

// =====================================================================
// Delegates — dual form (Native + Dynamic) for every public callback
// =====================================================================

// ---- OnGeneratePCMData -----------------------------------------------
// Fires during playback with the actual PCM samples the audio engine
// is producing, as normalized floats [-1.0, +1.0]. Useful for waveform
// visualization, lip-sync, VU metering. Runs on the audio render thread
// internally but broadcasts are marshaled to the game thread.
DECLARE_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsGeneratePCMDataNative,
    const TArray<float>&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsGeneratePCMData,
    const TArray<float>&, PCMData);

// ---- OnPopulateAudioData ---------------------------------------------
// Fires when new PCM data is appended to the wave's buffer (via
// AppendAudioDataFromRAW / AppendAudioDataFromMP3 / capture). Payload
// is the newly-appended samples as float32. Use for real-time analysis
// of incoming audio (e.g. waveform of mic input before it plays).
DECLARE_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsPopulateAudioDataNative,
    const TArray<float>&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsPopulateAudioData,
    const TArray<float>&, PopulatedAudioData);

// ---- OnPopulateAudioState --------------------------------------------
// Same trigger as OnPopulateAudioData but with no payload — cheaper for
// listeners that only need to know "new data landed" and will read the
// buffer themselves via GetPCMBuffer.
DECLARE_MULTICAST_DELEGATE(FOnInoAgentsPopulateAudioStateNative);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsPopulateAudioState);

// ---- OnAudioPlaybackFinished -----------------------------------------
// Fires once per stream after the wave's PCM buffer has been fully
// played through. Only fires when SetStopSoundOnPlaybackFinish(true) —
// otherwise the wave plays silence indefinitely waiting for more data.
DECLARE_MULTICAST_DELEGATE(FOnInoAgentsAudioPlaybackFinishedNative);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsAudioPlaybackFinished);

// ---- OnAudioError ----------------------------------------------------
// Fires on decode failure, capture failure, or other non-recoverable
// errors. The wave remains valid; the caller decides whether to retry,
// reset, or abandon.
DECLARE_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsAudioErrorNative,
    const FString&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsAudioError,
    FString, ErrorMessage);

// ---- OnPreAllocateAudioDataResult ------------------------------------
// Single-shot callback from PreAllocateAudioData. Not multicast — it's
// a completion notification for a specific call, not an event stream.
DECLARE_DELEGATE_OneParam(
    FOnInoAgentsPreAllocateAudioDataResultNative,
    bool /*bSucceeded*/);
DECLARE_DYNAMIC_DELEGATE_OneParam(
    FOnInoAgentsPreAllocateAudioDataResult,
    bool, bSucceeded);

// ---- OnGetAvailableAudioInputDevicesResult ---------------------------
// Single-shot callback from the static device-enumeration helper on
// UInoAgentsCapturableSoundWave. Not multicast — device enumeration
// is a one-shot query.
DECLARE_DELEGATE_OneParam(
    FOnInoAgentsGetAvailableAudioInputDevicesResultNative,
    const TArray<FInoAgentsAudioInputDeviceInfo>&);
DECLARE_DYNAMIC_DELEGATE_OneParam(
    FOnInoAgentsGetAvailableAudioInputDevicesResult,
    const TArray<FInoAgentsAudioInputDeviceInfo>&, AvailableDevices);

// ---- OnCaptureStarted / OnCaptureStopped -----------------------------
// Fire when a capture stream opens / closes on UInoAgentsCapturableSoundWave.
DECLARE_MULTICAST_DELEGATE(FOnInoAgentsCaptureStartedNative);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsCaptureStarted);

DECLARE_MULTICAST_DELEGATE(FOnInoAgentsCaptureStoppedNative);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsCaptureStopped);
