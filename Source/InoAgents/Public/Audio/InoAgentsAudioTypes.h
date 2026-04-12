// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoAgentsAudioTypes.generated.h"

/**
 * Audio encoding (bit depth / compression) passed to
 * UInoAgentsStreamingAudioComponent::FeedAudioBytes.
 *
 * IMPORTANT: this enum only describes HOW each sample is encoded in the
 * byte stream. It does NOT cover sample rate or channel count — those
 * are separate concepts:
 *
 *   - Bit depth / encoding      -> this enum   (PcmInt16, PcmFloat32, Mp3)
 *   - Sample rate (Hz)          -> SetPcmFormat(rate, channels)
 *   - Channel count (mono / stereo) -> SetPcmFormat(rate, channels)
 *
 * For PCM streams the caller must configure the sample rate + channel
 * count via SetPcmFormat BEFORE the first FeedAudioBytes because raw
 * PCM bytes don't carry that metadata. For MP3 the component auto-
 * detects both from the first decoded frame header and SetPcmFormat
 * is ignored.
 *
 * Obsolete / pro-audio-only formats (int8, int24, int32) are
 * deliberately NOT exposed. If you genuinely need them, convert to
 * PcmInt16 or PcmFloat32 on your side before calling FeedAudioBytes.
 */
UENUM(BlueprintType)
enum class EInoAgentsAudioFormat : uint8
{
    /** Signed 16-bit integer PCM, little-endian, interleaved for stereo.
     *  Two bytes per sample per channel. This is the universal standard
     *  for runtime audio byte streams and the native input format for
     *  USoundWaveProcedural — no conversion happens, the bytes queue
     *  straight into the audio engine. */
    PcmInt16            UMETA(DisplayName = "PCM 16-bit signed (int16 LE)"),

    /** IEEE 754 single-precision float PCM, interleaved for stereo.
     *  Four bytes per sample per channel. Samples are expected to be
     *  in the [-1.0, +1.0] range; out-of-range samples are clamped.
     *  Converted to int16 internally before being queued. Common
     *  output format for DSP pipelines and some TTS libraries. */
    PcmFloat32          UMETA(DisplayName = "PCM 32-bit float (-1.0 to +1.0)"),

    /** MPEG-1 / 2 / 2.5 Layer III. Sample rate and channel count are
     *  auto-detected from the first MP3 frame header — SetPcmFormat
     *  is ignored for MP3 streams. */
    Mp3                 UMETA(DisplayName = "MP3"),
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

/** Fires during audio playback with PCM sample data as normalized
 *  floats (-1.0 to 1.0). Use for visualizations (waveform, lip sync,
 *  VU meter, etc.). Batch size controlled by NumVisualizationSamples
 *  on the audio component. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoAgentsGeneratePCMData,
    const TArray<float>&, PCMData);
