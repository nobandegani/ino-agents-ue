// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoAgentsAudioTypes.generated.h"

/**
 * Audio encoding passed to UInoAgentsStreamingAudioComponent::FeedAudioBytes.
 *
 * PCM bytes do not carry sample-rate / channel-count metadata — callers
 * feeding PCM must configure those via SetPcmFormat BEFORE the first
 * FeedAudioBytes call. MP3 bytes carry the sample rate and channel count
 * in every frame header, so the component auto-detects them from the
 * first successfully-decoded frame.
 */
UENUM(BlueprintType)
enum class EInoAgentsAudioFormat : uint8
{
    /** Signed 16-bit little-endian PCM, interleaved for stereo. Default
     *  sample rate 44100 Hz, default channel count 1; override via
     *  UInoAgentsStreamingAudioComponent::SetPcmFormat. */
    Pcm16           UMETA(DisplayName = "PCM int16 LE"),

    /** MPEG-1 / 2 / 2.5 Layer III. Sample rate and channel count are
     *  auto-detected from the first MP3 frame header. */
    Mp3             UMETA(DisplayName = "MP3"),
};

/** Fires once per stream when enough decoded PCM is queued for playback
 *  to start. For PCM streams this is the first FeedAudioBytes with a
 *  non-empty buffer; for MP3 streams this is the first successfully-
 *  decoded frame. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsAudioReadyToPlay);

/** Fires once per stream after the caller has called FinalizeStream AND
 *  USoundWaveProcedural's queue has fully drained. Not mutually exclusive
 *  with StopAndReset — StopAndReset does NOT fire this delegate. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoAgentsAudioFinished);

/** Fires once per stream on any failure: MP3 decode error, malformed
 *  input, or USoundWaveProcedural misconfiguration. After OnError fires
 *  the stream is considered finished — subsequent FeedAudioBytes calls
 *  start a new stream. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsAudioError,
    FString, ErrorMessage);
