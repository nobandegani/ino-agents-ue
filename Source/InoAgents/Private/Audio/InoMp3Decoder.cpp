// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoMp3Decoder.h"

// minimp3 is a single-header C library. This is the ONE translation
// unit in the entire plugin that enables its implementation; every
// other TU that touches MP3 goes through the wrapper functions below
// rather than including minimp3.h directly. Keeps the decoder's ~1900
// lines of SSE / integer-math internals out of public includes.
//
// Compile-time knobs:
//   MINIMP3_IMPLEMENTATION  emit function bodies (one TU only)
//   MINIMP3_ONLY_MP3        skip MP1/MP2 decoders, trims the binary
//   MINIMP3_NO_STDIO        we never read files ourselves
//
// minimp3 is warning-clean under MSVC /W4 on the pinned commit; if a
// future update introduces warnings we can wrap the include in a
// push/pop pragma block, but today nothing is needed.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3.h"

/**
 * Opaque decoder state. Held by the caller via TPimplPtr so it
 * destructs cleanly when the owning component goes away.
 *
 * InputBuffer accumulates bytes that arrived via Feed() but haven't
 * been consumed by a complete MP3 frame yet. Every Feed call appends
 * to it, decodes as many complete frames as possible, then shifts the
 * unconsumed tail back to the front of the buffer (cheap — MP3 frame
 * headers are small so the tail is usually under a few KB).
 */
class FInoMp3DecodeState
{
public:
    FInoMp3DecodeState()
    {
        mp3dec_init(&Decoder);
    }

    mp3dec_t Decoder;

    /** Bytes seen by Feed() that haven't completed a frame yet. */
    TArray<uint8> InputBuffer;

    /** True once the first frame with valid (hz, channels) has
     *  parsed, so subsequent Feed results don't keep re-reporting
     *  the rate. */
    bool bFormatReported = false;
};

TPimplPtr<FInoMp3DecodeState> InoMp3::CreateState()
{
    return MakePimpl<FInoMp3DecodeState>();
}

FInoMp3DecodeResult InoMp3::Feed(
    FInoMp3DecodeState& State,
    const TArray<uint8>&      Bytes)
{
    FInoMp3DecodeResult Out;

    // Append incoming bytes to the rolling buffer. Empty Bytes is a
    // valid drain call — we still try to decode any frame that was
    // already buffered from a prior Feed.
    if (Bytes.Num() > 0)
    {
        State.InputBuffer.Append(Bytes);
    }

    // Decode in a loop until minimp3 can't find another complete frame
    // in the buffer. mp3dec_decode_frame returns:
    //   samples per channel > 0  -> a frame decoded successfully
    //   0 with frame_bytes > 0   -> skipped junk bytes (e.g. ID3 tag)
    //   0 with frame_bytes == 0  -> not enough input for a frame yet
    int32 Cursor = 0;
    while (true)
    {
        mp3dec_frame_info_t Info = {};

        // minimp3 may need up to ~1152*2 int16 samples of scratch per
        // frame. Give it a fresh scratch each iteration and copy the
        // successful output into Out.Pcm at the end.
        int16 FrameScratch[MINIMP3_MAX_SAMPLES_PER_FRAME] = {};

        const uint8* CursorPtr = State.InputBuffer.GetData() + Cursor;
        const int32  Remaining = State.InputBuffer.Num() - Cursor;

        if (Remaining <= 0)
        {
            break;
        }

        const int Samples = mp3dec_decode_frame(
            &State.Decoder,
            CursorPtr, Remaining,
            FrameScratch,
            &Info);

        if (Samples > 0)
        {
            // Frame decoded. Copy its PCM into the output. Info.channels
            // is 1 for mono, 2 for stereo; Samples is the per-channel
            // count, so the interleaved int16 count is Samples * channels.
            const int32 InterleavedCount = Samples * FMath::Max(Info.channels, 1);
            Out.Pcm.Append(FrameScratch, InterleavedCount);

            // Report the format exactly once per stream so the caller
            // can configure its sink before the first sample lands.
            if (!State.bFormatReported && Info.hz > 0 && Info.channels > 0)
            {
                Out.SampleRate   = Info.hz;
                Out.NumChannels  = Info.channels;
                State.bFormatReported = true;
            }

            Cursor += Info.frame_bytes;
            continue;
        }

        if (Info.frame_bytes > 0)
        {
            // Not a valid MP3 frame (e.g. ID3 tag, garbage, or a
            // junk byte). Skip past it and try again.
            Cursor += Info.frame_bytes;
            continue;
        }

        // frame_bytes == 0 AND samples == 0 means minimp3 needs more
        // input to complete the next frame. Bail the loop.
        break;
    }

    // Shift unconsumed tail to the front of InputBuffer so the next
    // Feed call starts fresh. RemoveAt is cheap when the removed range
    // is contiguous at the start.
    if (Cursor > 0)
    {
        State.InputBuffer.RemoveAt(0, Cursor);
    }

    return Out;
}
