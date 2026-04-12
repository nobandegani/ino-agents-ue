// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgentsSettings.h"

UInoAgentsSettings::UInoAgentsSettings()
{
    // Default model entries — public Hugging Face repos, no auth needed.
    Models.Add({
        TEXT("Gemma 4 E2B"),
        TEXT("gemma-4-E2B-it.litertlm"),
        TEXT("https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm")
    });
    Models.Add({
        TEXT("Gemma 4 E4B"),
        TEXT("gemma-4-E4B-it.litertlm"),
        TEXT("https://huggingface.co/litert-community/gemma-4-E4B-it-litert-lm/resolve/main/gemma-4-E4B-it.litertlm")
    });
}

FString UInoAgentsSettings::GetEffectiveElevenLabsBaseUrl() const
{
    FString Result = ElevenLabsBaseUrl.IsEmpty()
        ? FString(TEXT("https://api.elevenlabs.io"))
        : ElevenLabsBaseUrl;

    while (Result.EndsWith(TEXT("/")))
    {
        Result.LeftChopInline(1);
    }
    return Result;
}

const FLiteRtLmModelEntry* UInoAgentsSettings::FindModelByFileName(
    const FString& FileName) const
{
    for (const FLiteRtLmModelEntry& Entry : Models)
    {
        if (Entry.ModelFileName.Equals(FileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}
