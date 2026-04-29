// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNativeSettings.h"

UInoNeuTtsNativeSettings::UInoNeuTtsNativeSettings()
{
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
        FInoNeuTtsNanoNativeModelEntry Q4;
        Q4.DisplayName              = TEXT("NeuTTS Nano Q4");
        Q4.BackboneVariant          = EInoNeuTtsNanoNativeBackboneVariant::Q4;
        Q4.BackboneHuggingFaceRepoUrl = TEXT("https://huggingface.co/neuphonic/neutts-nano-q4-gguf");
        Q4.BackboneRevision         = TEXT("main");
        Q4.BackboneFileName         = TEXT("neutts-nano-Q4_0.gguf");
        Q4.CodecHuggingFaceRepoUrl  = TEXT("https://huggingface.co/neuphonic/neucodec-onnx-decoder");
        Q4.CodecRevision            = TEXT("main");
        Q4.CodecFileName            = TEXT("model.onnx");
        NeuTtsNanoNativeModels.Add(MoveTemp(Q4));
    }
}

const FInoNeuTtsNanoNativeModelEntry* UInoNeuTtsNativeSettings::FindNeuTtsNanoNativeModel(
    EInoNeuTtsNanoNativeBackboneVariant Variant) const
{
    // First match wins — same linear-scan / first-match-wins policy
    // as Chatterbox. NeuTTS Nano's variant count is tiny (Q4 only in
    // v1, Q8 as a future addition).
    for (const FInoNeuTtsNanoNativeModelEntry& Entry : NeuTtsNanoNativeModels)
    {
        if (Entry.BackboneVariant == Variant)
        {
            return &Entry;
        }
    }
    return nullptr;
}
