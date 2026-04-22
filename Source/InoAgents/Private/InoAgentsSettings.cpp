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

    // Default Chatterbox entries — same Resemble AI HF repo, different
    // quantization variants. Users can add entries for q4 / fp32 /
    // quantized in Project Settings the same way.
    //
    //   q4f16 — smallest (~510 MB total), fastest on mobile + AVX-512-FP16
    //     CPUs, but DirectML has op-coverage issues with this tier on
    //     dynamic-shape AR pipelines (MultiHeadAttention / Slice kernels
    //     fail at Run time — see FInoChatterboxPerformanceOptions docs
    //     on the per-session CPU overrides). CPU path on q4f16 is rock-
    //     solid, and it's the default ship-everywhere variant.
    //
    //   fp16 — ~1.5 GB total, half-precision weights + activations.
    //     Generally better-behaved on non-CPU ORT providers (DML, CUDA)
    //     than q4f16 — fewer of the quantization-specific kernel
    //     mismatches that bite q4f16. Worth trying when experimenting
    //     with DirectML.
    {
        FInoChatterboxModelEntry Q4F16;
        Q4F16.DisplayName        = TEXT("Chatterbox Turbo q4f16");
        Q4F16.Variant            = EInoChatterboxVariant::Q4F16;
        Q4F16.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        Q4F16.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(Q4F16));
    }
    {
        FInoChatterboxModelEntry FP16;
        FP16.DisplayName        = TEXT("Chatterbox Turbo fp16");
        FP16.Variant            = EInoChatterboxVariant::FP16;
        FP16.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        FP16.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(FP16));
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
