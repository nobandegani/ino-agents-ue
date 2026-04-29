// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTtsNanoNative/InoNeuTtsNanoNativeTypes.h"

#include "Misc/Paths.h"

FString NeuTtsNanoNativeVariantToString(EInoNeuTtsNanoNativeBackboneVariant Variant)
{
    switch (Variant)
    {
    case EInoNeuTtsNanoNativeBackboneVariant::Q4: return TEXT("q4");
    case EInoNeuTtsNanoNativeBackboneVariant::Q8: return TEXT("q8");
    }
    // Fall-through for forward-compat (new enum value added without a
    // case above — compiler won't warn in UE5's aggressive
    // strict-enum-casting but at least this is obvious at runtime).
    return TEXT("unknown");
}

FString NeuTtsNanoNativeResolveModelDir(EInoNeuTtsNanoNativeBackboneVariant Variant)
{
    // {PersistentDownloadDir}/InoAgents/Models/NeuTtsNanoNative/<variant>/
    //
    // PersistentDownloadDir resolves to:
    //   Win64: %LOCALAPPDATA%/<ProjectName>/Saved/PersistentDownloadDir/
    //   Android: /data/data/<package>/files/PersistentDownloadDir/
    //   (FPaths::ProjectPersistentDownloadDir() per-platform)
    //
    // Matches Chatterbox's layout under Models/Chatterbox/<variant>/,
    // just with the NeuTtsNanoNative/ subfolder swap.
    return FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"),
        TEXT("Models"),
        TEXT("NeuTtsNanoNative"),
        NeuTtsNanoNativeVariantToString(Variant));
}
