// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Chatterbox/InoChatterboxTypes.h"

#include "Misc/Paths.h"

FString ChatterboxVariantToString(EInoChatterboxVariant Variant)
{
    switch (Variant)
    {
        case EInoChatterboxVariant::Q4F16:     return TEXT("q4f16");
        case EInoChatterboxVariant::FP16:      return TEXT("fp16");
        case EInoChatterboxVariant::Q4:        return TEXT("q4");
        case EInoChatterboxVariant::FP32:      return TEXT("fp32");
        case EInoChatterboxVariant::Quantized: return TEXT("quantized");
    }
    // Unreachable for a well-formed enum value; return the default so
    // downstream code doesn't get a garbage filename.
    return TEXT("q4f16");
}

FString ChatterboxVariantToFileSuffix(EInoChatterboxVariant Variant)
{
    // fp32 is the "baseline" variant in the upstream HF repo — files
    // are named speech_encoder.onnx (no suffix). Every other variant
    // has a "_<variant>" suffix. Must match on BOTH sides (URL + local
    // filename) so ORT's external-data lookup for the .onnx_data
    // companion resolves correctly.
    if (Variant == EInoChatterboxVariant::FP32)
    {
        return FString();
    }
    return FString::Printf(TEXT("_%s"), *ChatterboxVariantToString(Variant));
}

bool ChatterboxVariantFromString(
    const FString& InString,
    EInoChatterboxVariant& OutVariant)
{
    // Canonical lowercase filenames upstream — accept case-insensitive
    // so "Q4F16" or "FP16" typed in Blueprint still match.
    if (InString.Equals(TEXT("q4f16"), ESearchCase::IgnoreCase))
    {
        OutVariant = EInoChatterboxVariant::Q4F16;
        return true;
    }
    if (InString.Equals(TEXT("fp16"), ESearchCase::IgnoreCase))
    {
        OutVariant = EInoChatterboxVariant::FP16;
        return true;
    }
    if (InString.Equals(TEXT("q4"), ESearchCase::IgnoreCase))
    {
        OutVariant = EInoChatterboxVariant::Q4;
        return true;
    }
    if (InString.Equals(TEXT("fp32"), ESearchCase::IgnoreCase))
    {
        OutVariant = EInoChatterboxVariant::FP32;
        return true;
    }
    if (InString.Equals(TEXT("quantized"), ESearchCase::IgnoreCase))
    {
        OutVariant = EInoChatterboxVariant::Quantized;
        return true;
    }
    return false;
}

FString ChatterboxResolveVariantDir(EInoChatterboxVariant Variant)
{
    // Matches:
    //   - the layout produced by Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1
    //     (TargetDir = Saved\PersistentDownloadDir\InoAgents\Models\Chatterbox\<variant>)
    //   - the ResolveChatterboxDir() helper used by the Phase B smoke
    //     tests (InoChatterboxTest.cpp).
    //
    // FPaths::ProjectPersistentDownloadDir resolves correctly on both
    // desktop (Saved/PersistentDownloadDir/) and Android (the app's
    // data directory) without any platform-specific branching.
    return FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"),
        TEXT("Models"),
        TEXT("Chatterbox"),
        ChatterboxVariantToString(Variant));
}

bool ChatterboxVariantHasFp16Activations(EInoChatterboxVariant V)
{
    // fp16 group: both quantization tiers that ONNX-export with fp16
    // activations. The ONNX file's intermediate tensor dtypes are what
    // governs this — not the weight storage format. q4f16's 4-bit
    // weights get dequantized to fp16 at runtime, same activation
    // plane as vanilla fp16 export.
    switch (V)
    {
        case EInoChatterboxVariant::Q4F16:
        case EInoChatterboxVariant::FP16:
            return true;
        case EInoChatterboxVariant::Q4:
        case EInoChatterboxVariant::FP32:
        case EInoChatterboxVariant::Quantized:
            return false;
    }
    return false;
}

bool ChatterboxVariantsAreDtypeCompatible(
    EInoChatterboxVariant A, EInoChatterboxVariant B)
{
    // Compatible iff both variants share the same activation dtype
    // group. This is what ORT needs to hand tensors off between
    // sessions without an explicit cast.
    return ChatterboxVariantHasFp16Activations(A)
        == ChatterboxVariantHasFp16Activations(B);
}

void ChatterboxResolveSessionVariants(
    const FInoChatterboxModelConfig& Config,
    EInoChatterboxVariant&           OutSpeechEncoder,
    EInoChatterboxVariant&           OutEmbedTokens,
    EInoChatterboxVariant&           OutLanguageModel,
    EInoChatterboxVariant&           OutConditionalDecoder)
{
    if (Config.bUsePerSessionVariants)
    {
        OutSpeechEncoder      = Config.SpeechEncoderVariant;
        OutEmbedTokens        = Config.EmbedTokensVariant;
        OutLanguageModel      = Config.LanguageModelVariant;
        OutConditionalDecoder = Config.ConditionalDecoderVariant;
    }
    else
    {
        // Simple case: Variant applies to every session.
        OutSpeechEncoder      = Config.Variant;
        OutEmbedTokens        = Config.Variant;
        OutLanguageModel      = Config.Variant;
        OutConditionalDecoder = Config.Variant;
    }
}
