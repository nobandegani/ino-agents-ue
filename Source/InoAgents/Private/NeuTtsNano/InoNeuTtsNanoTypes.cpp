// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"

#include "Misc/Paths.h"

FString NeuTtsNanoVariantToString(EInoNeuTtsNanoBackboneVariant Variant)
{
    switch (Variant)
    {
    case EInoNeuTtsNanoBackboneVariant::Q4: return TEXT("q4");
    case EInoNeuTtsNanoBackboneVariant::Q8: return TEXT("q8");
    }
    // Fall-through for forward-compat (new enum value added without a
    // case above — compiler won't warn in UE5's aggressive
    // strict-enum-casting but at least this is obvious at runtime).
    return TEXT("unknown");
}

FString NeuTtsNanoResolveModelDir(EInoNeuTtsNanoBackboneVariant Variant)
{
    // {PersistentDownloadDir}/InoAgents/Models/NeuTtsNano/<variant>/
    //
    // PersistentDownloadDir resolves to:
    //   Win64: %LOCALAPPDATA%/<ProjectName>/Saved/PersistentDownloadDir/
    //   Android: /data/data/<package>/files/PersistentDownloadDir/
    //   (FPaths::ProjectPersistentDownloadDir() per-platform)
    //
    // Matches Chatterbox's layout under Models/Chatterbox/<variant>/,
    // just with the NeuTtsNano/ subfolder swap.
    return FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"),
        TEXT("Models"),
        TEXT("NeuTtsNano"),
        NeuTtsNanoVariantToString(Variant));
}
