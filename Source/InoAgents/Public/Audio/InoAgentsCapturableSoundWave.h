// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"
#include "Templates/PimplPtr.h"

#include "Audio/InoAgentsStreamingSoundWave.h"
#include "Audio/InoAgentsAudioTypes.h"

#include "InoAgentsCapturableSoundWave.generated.h"

class FInoAgentsAudioCaptureState;

/**
 * Streaming sound wave that accepts microphone input.
 *
 * Backed by Audio::FAudioCapture from the AudioCaptureCore module.
 * Captured frames arrive on the platform-specific capture thread as
 * float32 interleaved PCM, which we route straight through the
 * streaming append path (AppendAudioDataFromRAW) so the same
 * visualization / playback / drain infrastructure applies — capture
 * is literally "a remote source feeding the streaming wave".
 *
 * Typical use:
 *   Wave = UInoAgentsCapturableSoundWave::CreateCapturableSoundWave();
 *   Wave->StartCapture(-1);   // -1 = system default input device
 *   AudioComp->SetSound(Wave);
 *   AudioComp->Play();        // optional — skip if you only want to
 *                             //   inspect capture via delegates, no
 *                             //   monitoring playback
 *   // later:
 *   Wave->StopCapture();
 *
 * Platform support: Windows + Mac in v1 (via AudioCaptureRtAudio).
 * iOS / Android require additional backend modules and permission
 * handling — StartCapture returns false on unsupported platforms
 * and broadcasts OnAudioError with a clear message.
 */
UCLASS(BlueprintType, Category = "InoAgents|Audio")
class INOAGENTS_API UInoAgentsCapturableSoundWave : public UInoAgentsStreamingSoundWave
{
    GENERATED_BODY()

public:
    UInoAgentsCapturableSoundWave(const FObjectInitializer& ObjectInitializer);

    //~ UObject interface
    virtual void BeginDestroy() override;
    //~ End UObject interface

    // =================================================================
    // Factory
    // =================================================================

    /** Allocate a new capturable wave. Game thread only. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Capture")
    static UInoAgentsCapturableSoundWave* CreateCapturableSoundWave();

    // =================================================================
    // Device enumeration
    // =================================================================

    /**
     * Query the system for available audio input devices. The result
     * callback fires on the game thread once enumeration completes.
     * The enumeration itself is quick (single-digit ms on Windows);
     * the async indirection exists so the call signature stays
     * consistent with platform backends that need to touch a
     * background queue (e.g. a future Android impl).
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Capture")
    static void GetAvailableAudioInputDevices(
        const FOnInoAgentsGetAvailableAudioInputDevicesResult& Result);

    /** Native (C++) variant of GetAvailableAudioInputDevices. */
    static void GetAvailableAudioInputDevices(
        const FOnInoAgentsGetAvailableAudioInputDevicesResultNative& Result);

    // =================================================================
    // Capture control
    // =================================================================

    /**
     * Open a capture stream on the given device and start feeding
     * samples into the wave's buffer. Returns false immediately if
     * the platform has no capture backend, the device can't be
     * opened, or the wave is already capturing.
     *
     * @param DeviceId  Index of the device to open, as returned in
     *                  FInoAgentsAudioInputDeviceInfo::DeviceId (parsed
     *                  as an integer). Pass -1 for the system default
     *                  input.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Capture")
    bool StartCapture(int32 DeviceId);

    /** Close the capture stream. Idempotent — no-op if not capturing. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Capture")
    void StopCapture();

    /**
     * Mute or unmute the capture input. While muted, the capture
     * thread still fires but we append zero-filled frames so
     * visualization / downstream delegates see "silence" rather
     * than an interrupted stream.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Capture")
    bool ToggleMute(bool bMute);

    /** True while a capture stream is open and receiving frames. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Capture")
    bool IsCapturing() const;

    // =================================================================
    // Delegates
    // =================================================================

    /** C++-only native variant of OnCaptureStarted. */
    FOnInoAgentsCaptureStartedNative OnCaptureStartedNative;

    /** Fires on the game thread right after StartCapture succeeds. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio|Capture")
    FOnInoAgentsCaptureStarted OnCaptureStarted;

    /** C++-only native variant of OnCaptureStopped. */
    FOnInoAgentsCaptureStoppedNative OnCaptureStoppedNative;

    /** Fires on the game thread after the capture stream closes
     *  (whether via StopCapture or an error). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio|Capture")
    FOnInoAgentsCaptureStopped OnCaptureStopped;

private:
    /**
     * Platform-specific capture state held opaquely so the header
     * stays clean of AudioCaptureCore types on platforms that don't
     * support capture.
     */
    TPimplPtr<FInoAgentsAudioCaptureState> CaptureState;

    /** True while an open capture stream is forwarding frames.
     *  Written on the game thread (Start/Stop/BeginDestroy), read on
     *  the platform capture thread inside the OnCapture lambda — so
     *  it needs atomic access to avoid a torn read. */
    TAtomic<bool> bIsCapturing{false};

    /** Muted state. Same cross-thread pattern as bIsCapturing. When
     *  true, the OnCapture callback appends zero-filled frames so
     *  downstream visualization sees silence rather than an
     *  interrupted stream. */
    TAtomic<bool> bMuted{false};
};
