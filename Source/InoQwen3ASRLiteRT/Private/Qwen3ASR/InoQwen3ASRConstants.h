// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * Architecture constants for Qwen3-ASR-0.6B's `_5s_*` LiteRT export.
 *
 * Sources:
 *   - preprocessor_config.json  (Whisper feature extractor: n_fft, hop_length, n_mels)
 *   - generation_config.json    (eos_token_id, pad_token_id)
 *   - config.json               (audio_*_token_id, vocab_size, d_model, etc.)
 *   - .tflite signature dump    (encoder/decoder I/O shapes — Phase 1 LoadTest output)
 *
 * Changing any of these without re-exporting the .tflite (or pointing at a
 * different export) will desync caller and model — keep them locked.
 */
namespace InoQwen3ASR
{
    // ============================================================
    // Audio / mel-spectrogram parameters (Whisper convention)
    // ============================================================

    /** Sample rate the model was trained at. WAV input must be resampled to this. */
    constexpr int32 kSampleRate = 16000;

    /** Audio window per inference, in seconds. The `_5s_*` tflite variant. */
    constexpr int32 kAudioWindowSec = 5;

    /** Audio window in PCM samples = 80,000. */
    constexpr int32 kAudioWindowSamples = kSampleRate * kAudioWindowSec;

    /** STFT window size (samples). Whisper convention. */
    constexpr int32 kNFft = 400;

    /** STFT hop size (samples). 100 frames/sec. */
    constexpr int32 kHopLength = 160;

    /** Number of mel filterbank bins. */
    constexpr int32 kNMels = 128;

    /**
     * Mel frame count per 5-second window after STFT with center-padding +
     * the trailing-frame trim Whisper applies. Matches the encoder input's
     * 3rd dim from the signature dump:
     *     args_0  f32 [1, kNMels=128, kMelFrames=500]
     */
    constexpr int32 kMelFrames = 500;

    /** Mel filterbank lower frequency bound (Hz). Standard Slaney scale. */
    constexpr float kMelFmin = 0.0f;

    /** Mel filterbank upper frequency bound (Hz). Sample rate / 2. */
    constexpr float kMelFmax = 8000.0f;

    // ============================================================
    // Encoder / decoder I/O shapes (from .tflite signature dump)
    // ============================================================

    /**
     * Encoder output frame count. The audio encoder downsamples 500 mel
     * frames → 70 hidden states (~7× downsampling).
     *     encode out:  f32 [1, kEncoderHiddenFrames=70, kEncoderHiddenDim=1024]
     */
    constexpr int32 kEncoderHiddenFrames = 70;

    /** Encoder + decoder hidden size (text decoder operates at 1024). */
    constexpr int32 kEncoderHiddenDim = 1024;

    /**
     * Maximum decoded token sequence length per .tflite call. The decoder
     * graph is exported with a fixed-shape input_ids buffer of this length;
     * we pad with kPadTokenId in unused positions and use attention_mask
     * to indicate which positions are real.
     *     decode in:  args_1  i32 [1, kDecoderMaxTokens=64]
     *                 args_2  i32 [1, kDecoderMaxTokens=64]   (attention mask)
     *     decode out: output  f32 [1, kDecoderMaxTokens=64, kVocabSize=151936]
     */
    constexpr int32 kDecoderMaxTokens = 64;

    /** Qwen3 tokenizer vocab size. */
    constexpr int32 kVocabSize = 151936;

    // ============================================================
    // Special token IDs (from generation_config.json + config.json)
    // ============================================================

    /** `<|endoftext|>` — pad + one of two EOS tokens. */
    constexpr int32 kPadTokenId = 151643;

    /** `<|endoftext|>` — first EOS option. Same value as pad in Qwen3. */
    constexpr int32 kEosTokenIdEndOfText = 151643;

    /** `<|im_end|>` — Qwen chat-template message terminator; second EOS option. */
    constexpr int32 kEosTokenIdImEnd = 151645;

    /** `<|im_start|>` — Qwen chat-template message opener. */
    constexpr int32 kImStartTokenId = 151644;

    /** `<|audio_start|>` — opens an audio segment in the chat template. */
    constexpr int32 kAudioStartTokenId = 151669;

    /** `<|audio_end|>` — closes an audio segment. */
    constexpr int32 kAudioEndTokenId = 151670;

    /**
     * `<|audio_pad|>` placeholder — in the source Qwen3-Omni model this gets
     * replaced with audio encoder hidden states. Our LiteRT export passes the
     * encoder hidden states as a separate `args_0` tensor instead, so we don't
     * need to inject these into input_ids ourselves.
     */
    constexpr int32 kAudioPlaceholderTokenId = 151676;

    // ============================================================
    // Decoder run policy
    // ============================================================

    /**
     * Hard cap on AR loop iterations per Transcribe() call. Equal to
     * kDecoderMaxTokens — we can't generate more than the .tflite's fixed
     * decoder buffer holds. To transcribe longer audio, run the encoder/
     * decoder over the next 5-second chunk and concatenate text.
     */
    constexpr int32 kMaxGeneratedTokens = kDecoderMaxTokens;

    // ============================================================
    // Decoder prompt prefix (verbatim chat_template.json expansion)
    // ============================================================

    // Plain-text token IDs for the chat-template structure pieces, looked
    // up directly in vocab.json:
    //   "system"    → 8948
    //   "user"      → 872
    //   "assistant" → 77091
    //   "\n"        → 198    (byte 10 → GPT-2 codepoint 256+10 = 'Ċ')
    constexpr int32 kSystemTokenId    = 8948;
    constexpr int32 kUserTokenId      = 872;
    constexpr int32 kAssistantTokenId = 77091;
    constexpr int32 kNewlineTokenId   = 198;

    /**
     * Decoder prompt prefix produced by chat_template.json with an empty
     * system message and a single audio user-message. The audio content
     * itself flows through cross-attention from the encoder hidden states;
     * the <|audio_*|> tokens here are placeholder markers the model expects
     * to see in the prompt structure.
     *
     * Equivalent to:
     *   <|im_start|>system\n<|im_end|>\n
     *   <|im_start|>user\n<|audio_start|><|audio_pad|><|audio_end|><|im_end|>\n
     *   <|im_start|>assistant\n
     *
     * 16 tokens total → leaves kDecoderMaxTokens - 16 = 48 positions for
     * the actual transcription before the buffer is full.
     */
    inline TArrayView<const int32> GetDecoderPromptPrefix()
    {
        static const int32 PrefixIds[] = {
            kImStartTokenId,       // <|im_start|>
            kSystemTokenId,        // system
            kNewlineTokenId,       // \n
            kEosTokenIdImEnd,      // <|im_end|>
            kNewlineTokenId,       // \n
            kImStartTokenId,       // <|im_start|>
            kUserTokenId,          // user
            kNewlineTokenId,       // \n
            kAudioStartTokenId,    // <|audio_start|>
            kAudioPlaceholderTokenId, // <|audio_pad|>
            kAudioEndTokenId,      // <|audio_end|>
            kEosTokenIdImEnd,      // <|im_end|>
            kNewlineTokenId,       // \n
            kImStartTokenId,       // <|im_start|>
            kAssistantTokenId,     // assistant
            kNewlineTokenId,       // \n
        };
        return TArrayView<const int32>(PrefixIds, UE_ARRAY_COUNT(PrefixIds));
    }
}
