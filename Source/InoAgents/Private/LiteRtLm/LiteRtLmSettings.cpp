// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmSettings.h"

ULiteRtLmSettings::ULiteRtLmSettings()
{
    // Default model entries — public Hugging Face repos, no auth needed.
    // Users can add/remove/override in Project Settings.
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

const FLiteRtLmModelEntry* ULiteRtLmSettings::FindModelByFileName(
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
