// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/PimplPtr.h"

/**
 * Tiny wrapper around the vendored minimp3 decoder.
 *
 * The decoder state (FInoMp3DecodeState) is defined in the .cpp
 * that owns minimp3 so minimp3.h never leaks out of a single TU. The
 * public surface here is pimpl-friendly: callers hold a TPimplPtr to
 * the opaque state and call the free functions below.
 *
 * Usage:
 *     TPimplPtr<FInoMp3DecodeState> State = InoMp3::CreateState();
 *     for (each incoming byte chunk) {
 *         FInoMp3DecodeResult R = InoMp3::Feed(*State, Chunk);
 *         if (R.bError)           { fire OnError; break; }
 *         if (R.SampleRate > 0)   { configure sink rate / channels; }
 *         if (R.Pcm.Num() > 0)    { queue R.Pcm into sink; }
 *     }
 *     // optional final drain with empty input
 *     FInoMp3DecodeResult Tail = InoMp3::Feed(*State, {});
 */
class FInoMp3DecodeState;

/** Result of one Feed call. */
struct FInoMp3DecodeResult
{
    /** Decoded PCM samples, int16 interleaved. Zero-length is valid
     *  (meaning: not enough bytes queued to complete a frame yet). */
    TArray<int16> Pcm;

    /** Detected sample rate from the first frame that parses in this
     *  stream. Non-zero only the first time a frame decodes; subsequent
     *  Feed calls return 0 for SampleRate / NumChannels because the
     *  caller is expected to have already configured its sink. */
    int32 SampleRate  = 0;
    int32 NumChannels = 0;

    /** True if the decoder reported a fatal error on this feed. Caller
     *  should stop decoding and surface ErrorMessage via OnError. */
    bool    bError = false;
    FString ErrorMessage;
};

namespace InoMp3
{
    /** Allocate a fresh decoder state. Cheap — just zeroes the state. */
    TPimplPtr<FInoMp3DecodeState> CreateState();

    /**
     * Consume some bytes. Appends them to the state's internal input
     * buffer, then decodes as many complete frames as possible. Bytes
     * that don't fit a complete frame stay in the buffer for the next
     * Feed call.
     *
     * Passing an empty Bytes array is valid: it acts as a drain call,
     * decoding any final frame that's already fully buffered from a
     * prior Feed.
     */
    FInoMp3DecodeResult Feed(
        FInoMp3DecodeState& State,
        const TArray<uint8>&      Bytes);
}
