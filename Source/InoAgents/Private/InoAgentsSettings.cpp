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
    // q4 — ~640 MB total, 4-bit weights but FP32 activations.
    // Notable for mobile: avoids the fp16 BiasGelu kernel gap on
    // Android's ORT 1.24.3 AAR that bites q4f16 on conditional_decoder
    // at graph-opt level 2 (Extended). If q4f16 is slow because we
    // had to drop to Basic on Android, q4 may let us go back up to
    // Extended and regain the fusion optimizations. Worth testing.
    {
        FInoChatterboxModelEntry Q4;
        Q4.DisplayName        = TEXT("Chatterbox Turbo q4");
        Q4.Variant            = EInoChatterboxVariant::Q4;
        Q4.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        Q4.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(Q4));
    }
    // fp32 — ~3.2 GB total, reference-quality weights + activations.
    // Rarely worth shipping (too big) but indispensable for
    // benchmarking — if fp32 is faster than a quantized variant on
    // your hardware, that's a strong signal the quantized tier is
    // hitting a slow / unoptimized kernel path.
    {
        FInoChatterboxModelEntry FP32;
        FP32.DisplayName        = TEXT("Chatterbox Turbo fp32");
        FP32.Variant            = EInoChatterboxVariant::FP32;
        FP32.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        FP32.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(FP32));
    }
    // quantized — ~1.0 GB total, INT8 throughout (unusual suffix
    // "_quantized" on the file names, matching the HF naming convention).
    // Quality varies more than the other variants sentence-to-sentence;
    // worth trying if you're CPU-bound and can tolerate occasional
    // artifacts.
    {
        FInoChatterboxModelEntry Quantized;
        Quantized.DisplayName        = TEXT("Chatterbox Turbo quantized (int8)");
        Quantized.Variant            = EInoChatterboxVariant::Quantized;
        Quantized.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        Quantized.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(Quantized));
    }

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
