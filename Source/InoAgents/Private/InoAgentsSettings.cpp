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

    // Default NeuTTS Nano entry — Q4 only in v1. Backbone GGUF from
    // neuphonic/neutts-nano-q4-gguf (195 MB, Qwen2-derived ~117M params).
    // Codec ONNX decoder from neuphonic/neucodec-onnx-decoder (783 MB,
    // shared across variants). Both revisions pin to main for now; a
    // concrete commit SHA can be substituted when we want reproducibility.
    //
    // Q8 is enum-declared but NOT seeded here — adding
    // neuphonic/neutts-nano-q8-gguf is a follow-up milestone. Users who
    // want Q8 can add a second entry themselves in Project Settings.
    {
        FInoNeuTtsNanoModelEntry Q4;
        Q4.DisplayName              = TEXT("NeuTTS Nano Q4");
        Q4.BackboneVariant          = EInoNeuTtsNanoBackboneVariant::Q4;
        Q4.BackboneHuggingFaceRepoUrl = TEXT("https://huggingface.co/neuphonic/neutts-nano-q4-gguf");
        Q4.BackboneRevision         = TEXT("main");
        Q4.BackboneFileName         = TEXT("neutts-nano-Q4_0.gguf");
        Q4.CodecHuggingFaceRepoUrl  = TEXT("https://huggingface.co/neuphonic/neucodec-onnx-decoder");
        Q4.CodecRevision            = TEXT("main");
        Q4.CodecFileName            = TEXT("model.onnx");
        NeuTtsNanoModels.Add(MoveTemp(Q4));
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

const FInoNeuTtsNanoModelEntry* UInoAgentsSettings::FindNeuTtsNanoModel(
    EInoNeuTtsNanoBackboneVariant Variant) const
{
    // Same linear-scan / first-match-wins policy as FindChatterboxModel.
    // NeuTTS Nano's variant count is tiny (Q4 only in v1, Q8 as a future
    // addition) so a hash map would be overkill.
    for (const FInoNeuTtsNanoModelEntry& Entry : NeuTtsNanoModels)
    {
        if (Entry.BackboneVariant == Variant)
        {
            return &Entry;
        }
    }
    return nullptr;
}
