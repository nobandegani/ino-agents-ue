// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSSettings.h"

#include "Misc/Paths.h"

UInoNeuTTSSettings::UInoNeuTTSSettings()
{
    // Empty defaults — user populates via Project Settings UI or by
    // adding entries to DefaultGame.ini. We deliberately do NOT seed
    // any default URLs because users host their own .litertlm /
    // .tflite artifacts (the canonical NeuTTS Nano weights aren't
    // distributed by us).
}

const FInoNeuTTSBackboneEntry* UInoNeuTTSSettings::FindBackbone(const FString& NameOrFileName) const
{
    if (NameOrFileName.IsEmpty())
    {
        return BackboneModels.Num() > 0 ? &BackboneModels[0] : nullptr;
    }
    for (const FInoNeuTTSBackboneEntry& Entry : BackboneModels)
    {
        if (Entry.DisplayName.Equals(NameOrFileName, ESearchCase::IgnoreCase) ||
            Entry.LocalFileName.Equals(NameOrFileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

const FInoNeuTTSDecoderEntry* UInoNeuTTSSettings::FindDecoder(const FString& NameOrFileName) const
{
    if (NameOrFileName.IsEmpty())
    {
        return DecoderModels.Num() > 0 ? &DecoderModels[0] : nullptr;
    }
    for (const FInoNeuTTSDecoderEntry& Entry : DecoderModels)
    {
        if (Entry.DisplayName.Equals(NameOrFileName, ESearchCase::IgnoreCase) ||
            Entry.LocalFileName.Equals(NameOrFileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

FString UInoNeuTTSSettings::GetModelsDir()
{
    return FPaths::Combine(FPaths::ProjectPersistentDownloadDir(),
                           TEXT("InoAgents"), TEXT("NeuTTS"));
}

FString UInoNeuTTSSettings::ResolveLocalPath(const FString& LocalFileName)
{
    return FPaths::Combine(GetModelsDir(), LocalFileName);
}
