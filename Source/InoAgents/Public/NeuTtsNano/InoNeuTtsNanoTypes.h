// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoNeuTtsNanoTypes.generated.h"

/**
 * NeuTTS Nano backbone variant.
 *
 * v1 ships Q4 only (195 MB GGUF, matches neutts-nano-Q4_0.gguf from
 * neuphonic/neutts-nano-q4-gguf). Q8 slot reserved for a follow-up
 * milestone that adds the neuphonic/neutts-nano-q8-gguf repo (~300 MB).
 * FP16 / BF16 are intentionally skipped — GGUF Q4/Q8 covers the same
 * quality ground with much better on-disk footprint.
 */
UENUM(BlueprintType)
enum class EInoNeuTtsNanoBackboneVariant : uint8
{
    Q4 UMETA(DisplayName = "Q4 (195 MB)"),
    Q8 UMETA(DisplayName = "Q8 (deferred)"),
};

/**
 * Project-Settings-visible entry for a NeuTTS Nano variant.
 *
 * NeuTTS Nano pulls from TWO HuggingFace repos per variant (unlike
 * Chatterbox which sources everything from one repo): the GGUF backbone
 * comes from neuphonic/neutts-nano-<variant>-gguf and the ONNX decoder
 * comes from neuphonic/neucodec-onnx-decoder (shared across variants).
 * So each entry carries two URL+revision+filename sets.
 *
 * UInoAgentsSettings::NeuTtsNanoModels holds one of these per variant.
 * Seeded in the UInoAgentsSettings constructor with the Q4 defaults.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoNeuTtsNanoModelEntry
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    FString DisplayName = TEXT("NeuTTS Nano Q4");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoBackboneVariant BackboneVariant = EInoNeuTtsNanoBackboneVariant::Q4;

    // --- Backbone (Qwen2-derived GGUF) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Backbone")
    FString BackboneHuggingFaceRepoUrl = TEXT("https://huggingface.co/neuphonic/neutts-nano-q4-gguf");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Backbone")
    FString BackboneRevision = TEXT("main");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Backbone")
    FString BackboneFileName = TEXT("neutts-nano-Q4_0.gguf");

    /** Optional lowercase-hex SHA-256 of the backbone file. Empty disables
     *  verification. Not enforced in v1 (tech debt — inherit from
     *  LiteRT-LM's pattern when the shared HTTP helper lands). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Backbone",
              meta = (DisplayName = "Expected Backbone SHA-256"))
    FString BackboneExpectedSha256;

    // --- Codec (NeuCodec ONNX decoder) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Codec")
    FString CodecHuggingFaceRepoUrl = TEXT("https://huggingface.co/neuphonic/neucodec-onnx-decoder");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Codec")
    FString CodecRevision = TEXT("main");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Codec")
    FString CodecFileName = TEXT("model.onnx");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Codec",
              meta = (DisplayName = "Expected Codec SHA-256"))
    FString CodecExpectedSha256;
};

/**
 * Runtime-side load configuration. Chooses a variant; the URLs / file
 * names / SHA come from the matching UInoAgentsSettings entry.
 *
 * Blueprint consumers build this with a single enum pick; everything
 * else has sensible defaults.
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoNeuTtsNanoModelConfig
{
    GENERATED_BODY()

    /** Which variant to load. Matched against UInoAgentsSettings::FindNeuTtsNanoModel. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoBackboneVariant Variant = EInoNeuTtsNanoBackboneVariant::Q4;

    /** n_gpu_layers for the llama.cpp backbone. 0 = pure CPU, 99 = all
     *  layers on GPU (Vulkan on Win64), partial values = hybrid. Ignored
     *  on platforms without Vulkan (Android in v1). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "0", ClampMax = "99"))
    int32 NumGpuLayers = 0;

    /** Context-window size in tokens. 2048 matches NeuTTS's max_context
     *  default; raising it beyond the model's trained context is
     *  unsupported. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "512", ClampMax = "8192"))
    int32 NumContextTokens = 2048;
};

/**
 * Per-synthesis options. Defaults match the official Python pipeline
 * (neutts/neutts.py :: _infer_ggml: temperature=1.0, top_k=50,
 * max_tokens=max_context).
 */
USTRUCT(BlueprintType)
struct INOAGENTS_API FInoNeuTtsNanoSynthesisOptions
{
    GENERATED_BODY()

    /** Maximum speech tokens to generate before bailing. 2048 is the
     *  upstream default; typical utterances finish in 200-800 tokens. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "64", ClampMax = "4096"))
    int32 MaxNewTokens = 2048;

    /** Top-K sampling cutoff. NeuTTS default = 50. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "1", ClampMax = "500"))
    int32 TopK = 50;

    /** Softmax temperature. NeuTTS default = 1.0. Lower = more
     *  deterministic / flat prosody; higher = more variation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "0.1", ClampMax = "2.0"))
    float Temperature = 1.0f;

    /** Random seed for the sampler. -1 = non-deterministic (uses a
     *  time-based seed internally). Fix to a positive value for
     *  reproducible regression tests. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    int32 Seed = -1;
};

/**
 * One file in the NeuTTS Nano download queue — internal detail of the
 * auto-download flow. Declared at namespace scope (not nested in
 * UInoNeuTtsNanoSubsystem) so the build-queue helper in the subsystem's
 * .cpp anonymous namespace can reference it without friending.
 *
 * Mirrors FInoChatterboxDownloadFile — same semantics, different
 * containing subsystem. The duplication is flagged tech debt; a future
 * refactor will factor both into Private/InoHttpDownload/FDownloadFile.
 */
struct FInoNeuTtsNanoDownloadFile
{
    FString Url;
    FString TargetPath;       // absolute path on disk (not the .partial)
    bool    bRequired = true; // NeuTTS always-required; false not used in v1
    int64   ExpectedBytes = -1;
    int64   BytesWritten = 0;
    bool    bDone = false;
};

// ============================================================================
// Delegates — FString BY VALUE throughout (strict BindDynamic contract)
// ============================================================================

/**
 * Fired exactly once when LoadModelAsync resolves. bSuccess=true on
 * happy path; otherwise bSuccess=false with ErrorMessage set.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnInoNeuTtsNanoModelLoaded,
    bool,    bSuccess,
    FString, ErrorMessage);

/**
 * Fired exactly once when SynthesizeAsync resolves. On success,
 * PcmInt16LE carries raw 24 kHz mono int16 little-endian PCM bytes
 * (directly feedable into UStreamingSoundWave::AppendAudioDataFromRAW
 * via the RuntimeAudioImporter plugin — matches Chatterbox's output
 * contract).
 */
DECLARE_DYNAMIC_DELEGATE_FourParams(FOnInoNeuTtsNanoSynthesisComplete,
    bool,           bSuccess,
    TArray<uint8>,  PcmInt16LE,
    int32,          SampleRate,
    FString,        ErrorMessage);

/**
 * Multicast download-progress signal. Percent is 0..100 (clamped).
 * BytesReceived counts on-disk bytes written. TotalBytes is -1 when
 * the aggregate total isn't yet known (HF CDN sometimes strips
 * Content-Length; the progress falls back to file-count counting
 * until GET headers fill in sizes).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnInoNeuTtsNanoDownloadProgress,
    float, Percent,
    int64, BytesReceived,
    int64, TotalBytes);

// ============================================================================
// Free-function helpers
// ============================================================================

/**
 * Returns "q4" / "q8" (or similar short tokens) for a variant. Used to
 * build variant-scoped paths and directory names.
 */
INOAGENTS_API FString NeuTtsNanoVariantToString(EInoNeuTtsNanoBackboneVariant Variant);

/**
 * Absolute directory where model files for a given variant live:
 *   {PersistentDownloadDir}/InoAgents/Models/NeuTtsNano/{variant}/
 *
 * Matches the layout UInoNeuTtsNanoSubsystem uses as both its download
 * target and its model-load source. The shape mirrors Chatterbox's
 * per-variant directory pattern — makes a future multi-variant install
 * (Q4 + Q8 coexisting) trivial.
 */
INOAGENTS_API FString NeuTtsNanoResolveModelDir(EInoNeuTtsNanoBackboneVariant Variant);
