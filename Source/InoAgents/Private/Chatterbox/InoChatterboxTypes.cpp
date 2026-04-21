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
