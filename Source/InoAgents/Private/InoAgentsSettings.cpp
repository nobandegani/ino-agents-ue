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

    // Default Chatterbox entry — pulls q4f16 (the plugin default) from
    // the canonical Resemble AI HuggingFace repo. Users who want fp16
    // for desktop quality can add a second entry under Project Settings
    // pointing at the same repo with a different Variant.
    {
        FInoChatterboxModelEntry ChatterboxDefault;
        ChatterboxDefault.DisplayName        = TEXT("Chatterbox Turbo q4f16");
        ChatterboxDefault.Variant            = EInoChatterboxVariant::Q4F16;
        ChatterboxDefault.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        ChatterboxDefault.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(ChatterboxDefault));
    }
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

const FInoLiteRtLmModelEntry* UInoAgentsSettings::FindModelByFileName(
    const FString& FileName) const
{
    for (const FInoLiteRtLmModelEntry& Entry : Models)
    {
        if (Entry.ModelFileName.Equals(FileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

const FInoLiteRtLmModelEntry* UInoAgentsSettings::FindModel(
    const FString& NameOrFileName) const
{
    if (NameOrFileName.IsEmpty())
    {
        return nullptr;
    }

    // Prefer exact file-name match — if someone has a model named
    // "Gemma 4 E2B" as a display name but ALSO a different entry with
    // that literal file name, the file-name path should win (it's more
    // specific / authoritative).
    if (const FInoLiteRtLmModelEntry* Entry = FindModelByFileName(NameOrFileName))
    {
        return Entry;
    }

    // Fall back to display-name match.
    for (const FInoLiteRtLmModelEntry& Entry : Models)
    {
        if (Entry.DisplayName.Equals(NameOrFileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

const FInoChatterboxModelEntry* UInoAgentsSettings::FindChatterboxModel(
    EInoChatterboxVariant Variant) const
{
    // First match wins — multiple entries for the same variant is a
    // configuration error, but we don't actively complain about it
    // here (the Chatterbox subsystem will just use whichever entry
    // the user put first). Matches FindModelByFileName's style: a
    // trivial linear scan, cheap enough given the array will have
    // at most ~5 entries (one per quantization variant).
    for (const FInoChatterboxModelEntry& Entry : ChatterboxModels)
    {
        if (Entry.Variant == Variant)
        {
            return &Entry;
        }
    }
    return nullptr;
}
