// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxNativeSettings.h"

UInoChatterboxNativeSettings::UInoChatterboxNativeSettings()
{
    // Default Chatterbox entries — same Resemble AI HF repo, different
    // quantization variants. Users can add / remove entries in Project
    // Settings.
    //
    //   q4f16 — smallest (~510 MB total), fastest on mobile + AVX-512-FP16
    //     CPUs, but DirectML has op-coverage issues with this tier on
    //     dynamic-shape AR pipelines (MultiHeadAttention / Slice kernels
    //     fail at Run time — see FInoChatterboxTurboNativePerformanceOptions docs
    //     on the per-session CPU overrides). CPU path on q4f16 is rock-
    //     solid, and it's the default ship-everywhere variant.
    //
    //   fp16 — ~1.5 GB total, half-precision weights + activations.
    //     Generally better-behaved on non-CPU ORT providers (DML, CUDA)
    //     than q4f16 — fewer of the quantization-specific kernel
    //     mismatches that bite q4f16. Worth trying when experimenting
    //     with DirectML.
    {
        FInoChatterboxTurboNativeModelEntry Q4F16;
        Q4F16.DisplayName        = TEXT("Chatterbox Turbo q4f16");
        Q4F16.Variant            = EInoChatterboxTurboNativeVariant::Q4F16;
        Q4F16.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        Q4F16.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(Q4F16));
    }
    {
        FInoChatterboxTurboNativeModelEntry FP16;
        FP16.DisplayName        = TEXT("Chatterbox Turbo fp16");
        FP16.Variant            = EInoChatterboxTurboNativeVariant::FP16;
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
        FInoChatterboxTurboNativeModelEntry Q4;
        Q4.DisplayName        = TEXT("Chatterbox Turbo q4");
        Q4.Variant            = EInoChatterboxTurboNativeVariant::Q4;
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
        FInoChatterboxTurboNativeModelEntry FP32;
        FP32.DisplayName        = TEXT("Chatterbox Turbo fp32");
        FP32.Variant            = EInoChatterboxTurboNativeVariant::FP32;
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
        FInoChatterboxTurboNativeModelEntry Quantized;
        Quantized.DisplayName        = TEXT("Chatterbox Turbo quantized (int8)");
        Quantized.Variant            = EInoChatterboxTurboNativeVariant::Quantized;
        Quantized.HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");
        Quantized.Revision           = TEXT("main");
        ChatterboxModels.Add(MoveTemp(Quantized));
    }
}

const FInoChatterboxTurboNativeModelEntry* UInoChatterboxNativeSettings::FindChatterboxModel(
    EInoChatterboxTurboNativeVariant Variant) const
{
    // First match wins — multiple entries for the same variant is a
    // configuration error, but we don't actively complain about it here.
    // Linear scan is cheap given the array will have at most ~5 entries
    // (one per quantization variant).
    for (const FInoChatterboxTurboNativeModelEntry& Entry : ChatterboxModels)
    {
        if (Entry.Variant == Variant)
        {
            return &Entry;
        }
    }
    return nullptr;
}
