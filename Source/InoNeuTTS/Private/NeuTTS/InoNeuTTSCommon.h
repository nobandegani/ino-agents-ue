// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // EInoNeuTTSBackend, EInoNeuTTSActivationType

namespace InoNeuTTSNative
{
    // ----------------------------------------------------------------
    // Special token IDs baked into NeuTTS Nano's Llama-3-extended vocab.
    // Source: tokenizer_config.json in `models/nano/` +
    // `Plugins/InoLiteRT/Convert/NeuTTS/scripts/test_tts.py` constants.
    // ----------------------------------------------------------------
    constexpr int32 kTokenIdSpeechGenerationStart = 128260;  // <|SPEECH_GENERATION_START|>
    constexpr int32 kTokenIdSpeechGenerationEnd   = 128261;  // <|SPEECH_GENERATION_END|>  (stop)
    constexpr int32 kTokenIdSpeechBase            = 128262;  // <|speech_0|>
    constexpr int32 kNumSpeechCodes               = 65536;   // <|speech_0|> .. <|speech_65535|>

    // ----------------------------------------------------------------
    // NeuCodec output constants. Confirmed against
    // `Plugins/InoLiteRT/Convert/NeuCodec/scripts/convert_to_tflite.py`.
    // ----------------------------------------------------------------
    constexpr int32 kCodecHopLength = 480;     // PCM samples per FSQ frame
    constexpr int32 kSampleRate     = 24000;   // Hz, mono
    constexpr int32 kNumChannels    = 1;

    /** Resolve the on-disk path for a backbone (.litertlm) by DisplayName
     *  OR LocalFileName. Returns `<persistent>/InoAgents/NeuTTS/<LocalFileName>`
     *  if found in UInoNeuTTSSettings::BackboneModels; empty otherwise.
     *  Does NOT check that the file exists on disk. */
    FString ResolveBackbonePath(const FString& NameOrFileName = FString());

    /** Same shape for the decoder (.tflite) array. */
    FString ResolveDecoderPath(const FString& NameOrFileName = FString());

    /** Normalize whitespace in a phonemized string — collapse any run of
     *  whitespace to a single space and trim edges. Mirrors Neuphonic's
     *  reference `_to_phones`:
     *
     *      phones = phones.split()
     *      phones = " ".join(phones)
     *
     *  eSpeak occasionally emits leading/trailing whitespace per clause;
     *  if not normalized, BPE tokenization drifts vs the vendor reference. */
    FString NormalizePhones(const FString& Phones);

    // ----------------------------------------------------------------
    // Backend / activation enum → vendor C API mappings.
    // ----------------------------------------------------------------

    /** Convert EInoNeuTTSBackend → the static string LiteRT-LM expects
     *  in `litert_lm_engine_settings_create`'s backend_str arg. */
    const char* BackendToLiteRtLmString(EInoNeuTTSBackend Backend);

    /** Convert EInoNeuTTSBackend → the LiteRtHwAccelerators bit used by
     *  `LiteRtSetOptionsHardwareAccelerators`. Returns int32 so callers
     *  don't need to include the bare LiteRT headers here. */
    int32 BackendToLiteRtAcceleratorBit(EInoNeuTTSBackend Backend);

    /** Convert EInoNeuTTSActivationType → the int code LiteRT-LM expects
     *  in `litert_lm_engine_settings_set_activation_data_type` (0=F32,
     *  1=F16, 2=I16, 3=I8 per executor_settings_base.h). */
    int32 ActivationTypeToInt(EInoNeuTTSActivationType ActivationType);
}
