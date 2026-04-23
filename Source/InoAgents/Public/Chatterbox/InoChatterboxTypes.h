// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "InoChatterboxTypes.generated.h"

// ============================================================================
// Quantization variant
// ============================================================================

/**
 * Which ONNX quantization variant of Chatterbox Turbo to load.
 *
 * All five variants implement the same pipeline (speech_encoder →
 * embed_tokens → language_model → conditional_decoder); they differ only
 * in weight / activation precision. Total on-disk size for all four
 * runtime components, approximate:
 *
 *   fp32       ~3.2 GB    reference quality benchmark
 *   fp16       ~1.5 GB    essentially identical to fp32 on Chatterbox
 *   q4         ~640 MB    small quality drop; good on x86 without AVX-512 FP16
 *   q4f16      ~510 MB    small quality drop; default (smallest + fastest)
 *   Quantized  ~1.0 GB    INT8 everywhere; quality varies
 *
 * The variant string (lowercase — "q4f16", "fp16", etc.) is embedded in
 * each ONNX file's name, matches the on-disk directory name, and is
 * required to resolve the file paths at load time.
 *
 * See Plugins/InoAgents/Chatterbox/README.md for the full size /
 * quality tradeoff discussion.
 */
UENUM(BlueprintType)
enum class EInoChatterboxVariant : uint8
{
    // DisplayNames include the activation dtype suffix so the BP
    // dropdown makes cross-session dtype compatibility visible at a
    // glance. Two "groups" based on activation dtype:
    //   fp16 group:  Q4F16, FP16       (tensors between sessions are fp16)
    //   fp32 group:  Q4, FP32, Quantized (tensors between sessions are fp32)
    // Mixing sessions across groups fails at synth time with a dtype
    // mismatch (ORT won't automatically cast tensors crossing session
    // boundaries). Stay within one group when using per-session
    // variants.
    Q4F16     UMETA(DisplayName = "q4f16 — activations=fp16 (default, smallest)"),
    FP16      UMETA(DisplayName = "fp16 — activations=fp16 (desktop quality)"),
    Q4        UMETA(DisplayName = "q4 — activations=fp32 (small, wider kernel support)"),
    FP32      UMETA(DisplayName = "fp32 — activations=fp32 (benchmark)"),
    Quantized UMETA(DisplayName = "quantized — activations=fp32 (int8 weights)"),
};

/**
 * Returns true if two variants have matching activation dtypes and can
 * be safely combined across session boundaries without an explicit
 * dtype cast. Use when validating per-session variant selections.
 *
 *   q4f16 ↔ fp16              : compatible (both fp16)
 *   q4 ↔ fp32 ↔ quantized     : compatible (all fp32)
 *   q4f16/fp16  vs  q4/fp32/quantized : INCOMPATIBLE — synth will fail
 */
INOAGENTS_API bool ChatterboxVariantsAreDtypeCompatible(
    EInoChatterboxVariant A, EInoChatterboxVariant B);

/**
 * Returns true when the variant's activation tensors are fp16 (i.e.
 * the value would appear in the "fp16 group" — Q4F16 or FP16). Useful
 * to classify before mixing sessions.
 */
INOAGENTS_API bool ChatterboxVariantHasFp16Activations(EInoChatterboxVariant V);

/** Convert a variant enum to its canonical lowercase string
 *  ("q4f16", "fp16", "q4", "fp32", "quantized"). The returned value
 *  is used as the on-disk directory name and (for every variant
 *  EXCEPT fp32) as the filename suffix.
 *
 *  See ChatterboxVariantToFileSuffix for the filename-suffix form:
 *  Chatterbox Turbo's upstream HF repo uses NO suffix for the fp32
 *  variant (files are named e.g. "speech_encoder.onnx", not
 *  "speech_encoder_fp32.onnx"), and ORT's external-data protocol
 *  requires the local filenames to match what's embedded in the
 *  .onnx — so URLs + local paths both drop the suffix for fp32 even
 *  though the directory name still says "fp32". */
INOAGENTS_API FString ChatterboxVariantToString(EInoChatterboxVariant Variant);

/** Filename suffix (including the leading underscore) for a variant's
 *  ONNX / ONNX_DATA files. Empty for fp32 (matches HF's naming);
 *  "_" + variant string for every other variant.
 *
 *    fp32      → ""          → "speech_encoder.onnx"
 *    q4f16     → "_q4f16"    → "speech_encoder_q4f16.onnx"
 *    fp16      → "_fp16"     → "speech_encoder_fp16.onnx"
 *    q4        → "_q4"       → "speech_encoder_q4.onnx"
 *    quantized → "_quantized"→ "speech_encoder_quantized.onnx"
 *
 *  Use this for both URL construction (HF path) AND local filename
 *  construction (on-disk cache). They MUST match, otherwise ORT's
 *  external-data lookup will fail when loading the .onnx_data
 *  companion — the .onnx file embeds its companion path as a relative
 *  string. */
INOAGENTS_API FString ChatterboxVariantToFileSuffix(EInoChatterboxVariant Variant);

/** Parse a variant string (case-insensitive) into an enum value.
 *  Returns false on unknown input and leaves OutVariant untouched. */
INOAGENTS_API bool ChatterboxVariantFromString(
    const FString& InString,
    EInoChatterboxVariant& OutVariant);

// ============================================================================
// Path resolution
// ============================================================================

/**
 * Resolve the directory where a specific variant's staged files live.
 *
 * Always returns:
 *   <ProjectPersistentDownloadDir>/InoAgents/Models/Chatterbox/<variant>/
 *
 * Matches the layout produced by
 * Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 so dev-time
 * and runtime populate the same path. Does NOT check existence — that
 * is UInoChatterboxTtsSubsystem::IsModelDownloaded's job.
 */
INOAGENTS_API FString ChatterboxResolveVariantDir(EInoChatterboxVariant Variant);

// ============================================================================
// Load-time performance tuning
// ============================================================================

/**
 * Per-session ORT tuning knobs exposed at LoadModelsAsync time.
 * Applied to every one of the four ORT sessions the subsystem builds
 * (speech_encoder / embed_tokens / language_model / conditional_decoder),
 * so it's a bundle-wide setting, not per-session.
 *
 * Default-constructed (all fields at their defaults) gives ORT's
 * "figure it out" behaviour, which is usually fine but often
 * over-subscribes threads on hybrid CPUs (Intel 12th gen+, Arrow
 * Lake, Alder Lake, etc.) where E-cores drag down the AR loop.
 *
 * The #1 lever for CPU-only inference speed is IntraOpThreadCount.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxPerformanceOptions
{
    GENERATED_BODY()

    /** Number of threads ORT uses for parallel work INSIDE a single op
     *  (matmul parallelism, conv SIMD, etc.). 0 = ORT default (usually
     *  one per logical core — tends to over-subscribe hybrid CPUs).
     *
     *  For Chatterbox's AR loop the sweet spot is typically your
     *  physical P-core count:
     *    0   = ORT default              (a safe shot in the dark)
     *    4   = low-power / older laptops
     *    8   = most modern Intel/AMD desktops (Arrow Lake, Ryzen 7/9)
     *    16+ = HEDT / workstation / Threadripper
     *
     *  Pinning to P-core count on a hybrid CPU is often 10–20 %
     *  faster than letting ORT auto-pick — E-cores lag behind
     *  P-cores and the AR loop's slowest thread sets the pace. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance",
              meta = (ClampMin = "0", ClampMax = "128"))
    int32 IntraOpThreadCount = 0;

    /** Number of threads for PARALLEL execution of DIFFERENT ops in the
     *  same inference graph. 0 = ORT default, 1 = fully sequential.
     *
     *  For an AR-loop model like Chatterbox's language_model (each
     *  token depends on the previous), sequential is usually fastest —
     *  extra threads just add scheduling overhead. Leave at 1 unless
     *  you have a specific reason to change it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance",
              meta = (ClampMin = "0", ClampMax = "16"))
    int32 InterOpThreadCount = 1;

    /** Write ORT's per-op profiler output (onnxruntime_profile_*.json
     *  next to the executable) while the session is active. Costs
     *  5-10 % runtime overhead; off by default. Load the output in
     *  chrome://tracing or Perfetto to see what's actually dominating
     *  a given inference. Only turn on when diagnosing a specific
     *  slowdown. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance")
    bool bEnableOrtProfiling = false;

    /** Enable ORT's verbose logging (severity = VERBOSE / 0) for the
     *  four Chatterbox sessions. When on, ORT emits per-op graph
     *  decisions, provider-kernel-dispatch rationales, graph-transformer
     *  rewrites, and the exact DML_OPERATOR_*_DESC field that failed
     *  validation when DML rejects an op — indispensable when
     *  diagnosing DML kernel-support gaps.
     *
     *  Off by default (ORT default is WARNING which is quiet in the
     *  normal case). Expect the output log to grow substantially when
     *  on (~5-10 % performance overhead plus tens of thousands of log
     *  lines per session). Turn on only for specific debug runs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance")
    bool bEnableVerboseOrtLogging = false;

    // --- DirectML (Windows GPU/NPU acceleration) ---

    /** Use DirectML Execution Provider for GPU / NPU acceleration on
     *  Windows. Ignored on non-Windows platforms (DirectML is D3D12-
     *  based, no equivalent on Android / Linux / macOS).
     *
     *  When true, Chatterbox registers providers in priority order:
     *    { DirectMl, Cpu }
     *  Each op runs on DirectML (GPU or NPU — driver picks the
     *  fastest D3D12 device) when possible, falling back to CPU for
     *  the handful of ops DML doesn't cover.
     *
     *  When false, Windows uses the same CPU-only path Chatterbox
     *  has used historically. Useful for benchmarking, for debugging
     *  suspected DML-specific issues, or for shipping on very old
     *  Windows GPUs that predate D3D12.
     *
     *  Typical speedups vs. CPU-only on modern hardware:
     *    - Intel Arc iGPU (Xe-LPG on Core Ultra 200):  2-4×
     *    - NVIDIA RTX 3060+:                           5-10×
     *    - AMD RX 6000+:                               4-6×
     *    - Intel AI Boost NPU (24H2+ drivers):         variable,
     *      transparent routing; some ops fall back to iGPU/CPU.
     *
     *  Default true — if DirectML registration fails at runtime (no
     *  D3D12 device, corrupt DML install), ORT silently falls back
     *  to CPU. So leaving this on can only be equal to or faster
     *  than CPU-only, never slower (outside of DML's first-run
     *  shader-compile warmup hitch, which is one-time per session). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance")
    bool bPreferDirectMl = true;

    /** Windows-only: D3D12 adapter index for DirectML. 0 = default
     *  adapter (typically primary display GPU — integrated on laptops,
     *  discrete on desktops with dGPU). Values match
     *  IDXGIFactory::EnumAdapters order (check with dxdiag to pick
     *  deliberately).
     *
     *  Leave at 0 for most cases. Override only if you specifically
     *  want to target a non-default adapter (e.g. secondary dGPU,
     *  external Thunderbolt GPU, or an NPU that enumerates at a
     *  higher index on certain Windows 11 24H2+ driver builds).
     *
     *  Ignored if bPreferDirectMl is false or on non-Windows. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance",
              meta = (ClampMin = "0", ClampMax = "16"))
    int32 DirectMlAdapterIndex = 0;

    // --- Per-session "force CPU" opt-out ---
    //
    // ALL FOUR sessions default to CPU (b*OnCpu = true) because the
    // fancier execution provider on each platform (DirectML on Windows,
    // XNNPACK on Android) has demonstrated kernel-level bugs on at
    // least one Chatterbox session.
    //
    // Windows / DirectML (ORT 1.24.3). Empirically:
    //   - speech_encoder         → DML fails with E_INVALIDARG at
    //     MultiHeadAttention (MLOperatorAuthorImpl.cpp:2508)
    //   - embed_tokens           → DML fails with E_INVALIDARG at
    //     Slice on iter 1 of the AR loop (MLOperatorAuthorImpl.cpp:2853)
    //   - language_model         → DML produces silent numerical
    //     corruption on fp16 (outputs audible noise instead of speech);
    //     crashes on q4f16
    //   - conditional_decoder    → DML works correctly
    //
    // Confirmed upstream-side via a minimal Python repro using stock
    // onnxruntime-directml 1.24.3 — see Plugins/InoAgents/Chatterbox/
    // scripts/repro-dml-encoder.py. Microsoft has moved DirectML to
    // "sustained engineering"; these defects are unlikely to be fixed
    // upstream soon.
    //
    // Android / XNNPACK (ORT 1.24.3 AAR). Empirically:
    //   - speech_encoder         → XNNPACK's NhwcTransformer rewrites
    //     AveragePool into the 'com.ms.internal.nhwc' domain; the AAR
    //     is missing a kernel for com.ms.internal.nhwc.AveragePool(19)
    //     and session creation fails.
    //   - other sessions         → unverified; likely OK but not yet
    //     exercised on-device.
    //
    // Safe default is "all CPU" on both platforms. Flip the specific
    // flag(s) you want to experiment with:
    //   - Windows: bConditionalDecoderOnCpu=false + bPreferDirectMl=true
    //     is the verified-safe opt-in, ~15-20% faster overall.
    //   - Android: flip the non-speech-encoder flags to false one at a
    //     time and verify on-device. Opt into XNNPACK only where it
    //     actually loads + produces correct audio.
    //
    // When bPreferDirectMl is false on Windows, the per-session flags
    // are moot (everything's on CPU already). On Android the flags are
    // always consulted — Android has no "global disable" equivalent.

    /** Force speech_encoder to run on CPU. Default true — known broken
     *  on both accelerated paths:
     *    Windows/DML: E_INVALIDARG at MultiHeadAttention
     *    Android/XNNPACK: missing kernel for NHWC-transformed AveragePool(19) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance|Provider overrides")
    bool bSpeechEncoderOnCpu = true;

    /** Force embed_tokens to run on CPU. Default true — broken on DML
     *  (E_INVALIDARG at a Slice op on iter 1 of the AR loop); XNNPACK
     *  compatibility unverified. Safe to test on Android by flipping
     *  to false and watching the logs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance|Provider overrides")
    bool bEmbedTokensOnCpu = true;

    /** Force language_model to run on CPU. Default true — broken on DML
     *  (silent numerical corruption on fp16, crash on q4f16); XNNPACK
     *  compatibility unverified. This is the hot path, so accelerator
     *  support here would be the biggest speedup win. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance|Provider overrides")
    bool bLanguageModelOnCpu = true;

    /** Force conditional_decoder to run on CPU. Default true for safety
     *  consistency, but on Windows the decoder is the ONE Chatterbox
     *  session that DML handles correctly — flip this to false (with
     *  bPreferDirectMl=true) for a ~15-20% speedup. Android/XNNPACK
     *  compatibility unverified. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox|Performance|Provider overrides")
    bool bConditionalDecoderOnCpu = true;
};

// ============================================================================
// Load-time model configuration
// ============================================================================

/**
 * What UInoChatterboxTtsSubsystem::LoadModelsAsync needs to resolve
 * which files to download / load from disk.
 *
 * Simple case (default): set Variant to your chosen quantization and
 * leave bUsePerSessionVariants=false. All four Chatterbox sessions
 * (speech_encoder, embed_tokens, language_model, conditional_decoder)
 * load the same variant. This is equivalent to the pre-per-session
 * behavior and is what you want unless you have a specific reason to
 * mix.
 *
 * Advanced case: set bUsePerSessionVariants=true and pick a variant
 * per session. Useful for working around kernel-coverage gaps on
 * specific platforms — e.g. decoder on Q4 (fp32 activations) to dodge
 * fp16 BiasGelu kernel misses on Android while keeping the backbone
 * at Q4F16 to stay small.
 *
 * IMPORTANT — activation dtype compatibility:
 *   Cross-session tensor dtypes MUST match at session boundaries.
 *   Variants split into two groups by activation dtype:
 *
 *     fp16 group:  Q4F16, FP16
 *     fp32 group:  Q4, FP32, Quantized
 *
 *   You can mix freely WITHIN a group. Mixing ACROSS groups (e.g.
 *   Q4F16 encoder feeding into Q4 language_model) will fail at the
 *   first Run() with a dtype mismatch — ORT does NOT automatically
 *   cast tensors that cross session boundaries.
 *
 *   ChatterboxVariantsAreDtypeCompatible() classifies any pair. The
 *   subsystem logs a warning at load time if it detects a cross-group
 *   combination, so check the log if synth immediately errors.
 *
 * Lifecycle: only one load is resident in RAM at a time. Switch via
 * UnloadModels → LoadModelsAsync with a new config. Each session
 * loads independently and may need a fresh download if that session's
 * variant files aren't cached yet.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxModelConfig
{
    GENERATED_BODY()

    /** Default variant applied to all 4 sessions when
     *  bUsePerSessionVariants=false. Simplest option for typical use —
     *  set this, leave the per-session fields alone. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    EInoChatterboxVariant Variant = EInoChatterboxVariant::Q4F16;

    /** When false (default), the Variant above applies to all 4
     *  sessions. When true, the per-session fields below take over
     *  and Variant is ignored at load time.
     *
     *  Advanced: only enable if you actually need different variants
     *  for different sessions. See the struct-level comment for dtype
     *  compatibility rules — cross-group mixing fails at synth time. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    bool bUsePerSessionVariants = false;

    /** speech_encoder variant. Only consulted when
     *  bUsePerSessionVariants=true. Produces speaker conditioning
     *  tensors consumed by language_model (via concat with embed) and
     *  by conditional_decoder. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (EditCondition = "bUsePerSessionVariants"))
    EInoChatterboxVariant SpeechEncoderVariant = EInoChatterboxVariant::Q4F16;

    /** embed_tokens variant. Only consulted when
     *  bUsePerSessionVariants=true. Tiny session (a lookup table); its
     *  output dtype must match language_model's input dtype. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (EditCondition = "bUsePerSessionVariants"))
    EInoChatterboxVariant EmbedTokensVariant = EInoChatterboxVariant::Q4F16;

    /** language_model variant. Only consulted when
     *  bUsePerSessionVariants=true. Biggest session (~150-300 MB
     *  depending on variant). Expects fp16 or fp32 inputs matching the
     *  speech_encoder + embed_tokens outputs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (EditCondition = "bUsePerSessionVariants"))
    EInoChatterboxVariant LanguageModelVariant = EInoChatterboxVariant::Q4F16;

    /** conditional_decoder variant. Only consulted when
     *  bUsePerSessionVariants=true. Attention-heavy, the biggest
     *  wallclock contributor. Expects tensors matching the
     *  speech_encoder's speaker-conditioning output dtype. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (EditCondition = "bUsePerSessionVariants"))
    EInoChatterboxVariant ConditionalDecoderVariant = EInoChatterboxVariant::Q4F16;

    /** Per-session ORT tuning — thread counts, profiling toggle. See
     *  FInoChatterboxPerformanceOptions for the full doc on each field.
     *  Default-constructed = ORT auto, which is usually fine but not
     *  optimal on hybrid CPUs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FInoChatterboxPerformanceOptions Performance;
};

/**
 * Resolve the 4 per-session variants from a config. Honors the
 * bUsePerSessionVariants toggle:
 *   false → all 4 outputs = Config.Variant (simple case)
 *   true  → each output = its corresponding per-session field.
 *
 * Always fills all 4 outputs; never returns an error. The loader
 * calls this once at LoadModelsAsync time and threads the resolved
 * variants through download + load + subsystem state.
 */
INOAGENTS_API void ChatterboxResolveSessionVariants(
    const FInoChatterboxModelConfig& Config,
    EInoChatterboxVariant&           OutSpeechEncoder,
    EInoChatterboxVariant&           OutEmbedTokens,
    EInoChatterboxVariant&           OutLanguageModel,
    EInoChatterboxVariant&           OutConditionalDecoder);

// ============================================================================
// Per-utterance synthesis options
// ============================================================================

/**
 * Generation parameters for one SynthesizeAsync call. Forwarded
 * verbatim to FInoChatterboxRunner::FSynthesisOptions — this struct
 * exists as a USTRUCT so Blueprint graphs can set the values without
 * touching internal types.
 *
 * Defaults match Resemble AI's published reference script. Change with
 * care: RepetitionPenalty outside the 1.1–1.3 range visibly degrades
 * prosody, and MaxNewTokens over 1024 is wasted CPU because the AR loop
 * almost never emits that many speech tokens for a single utterance.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxSynthesisOptions
{
    GENERATED_BODY()

    /** Upper bound on speech tokens generated before the AR loop
     *  force-stops. Typical real utterances fit in 256–512 tokens;
     *  long narration may need up to 1024. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (ClampMin = "1", ClampMax = "1024"))
    int32 MaxNewTokens = 1024;

    /** Divisor applied to already-seen token logits so the model
     *  stops repeating itself. 1.0 disables; 1.2 is the reference
     *  default and the sweet spot for English speech. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox",
              meta = (ClampMin = "1.0", ClampMax = "2.0"))
    float RepetitionPenalty = 1.2f;
};

// ============================================================================
// Reference voice
// ============================================================================

/**
 * Reference audio that drives voice cloning for one SynthesizeAsync
 * call. Supplies the "speak like this voice" signal that the speech
 * encoder converts into speaker conditioning tensors.
 *
 * Chatterbox Turbo always needs SOME reference audio — the
 * architecture has speaker conditioning wired into every stage
 * (language_model + conditional_decoder both consume the encoder's
 * output tensors). You cannot generate "neutral" speech from text
 * alone. An empty FInoChatterboxVoice is accepted, but falls back to
 * the auto-downloaded default voice (see priority list below).
 *
 * Four input paths, in priority order:
 *
 *   1. WavFilePath — absolute or project-relative WAV file on disk.
 *      The subsystem reads it off the game thread via the internal
 *      InoChatterboxAudioIO reader. Must be 24 kHz mono PCM int16 or
 *      IEEE float32. Other sample rates / channel layouts cause
 *      SynthesizeAsync to error out with a clear message (no silent
 *      resampling — voice cloning quality is extremely sensitive to
 *      resample artifacts; do it properly offline, or supply a
 *      correctly-formatted clip).
 *
 *   2. ReferenceSamples — 24 kHz mono int16 PCM little-endian bytes.
 *      Used when the voice is already in memory (e.g. captured from
 *      the microphone in-engine, received over the network, loaded
 *      from a custom asset). Byte count must be a multiple of 2 —
 *      each int16 sample is two bytes. The subsystem converts to
 *      float32 internally before handing to the speech encoder.
 *      Ignored if WavFilePath is non-empty.
 *
 *   3. PrecomputedConditioningPath — RESERVED for Phase E. A future
 *      authoring step will bake (cond_emb, prompt_token,
 *      speaker_embeddings, speaker_features) into a .bin sidecar, and
 *      setting this path will skip the speech_encoder run entirely at
 *      synthesis time. **Setting this in Phase D causes
 *      SynthesizeAsync to error** — the field is declared now so
 *      Blueprint graphs that wire it today survive the Phase E
 *      implementation change without needing a node edit.
 *
 *   4. (fallback) Default voice at <variant_dir>/default_voice.wav —
 *      auto-downloaded alongside the model files (714 KB, MIT-licensed,
 *      24 kHz mono, from onnx-community/chatterbox-ONNX). Used when
 *      all three fields above are empty. Makes the minimum
 *      "LoadModelsAsync + SynthesizeAsync" flow a single no-args call
 *      for prototyping. The file is marked bRequired=false in the
 *      download queue, so a 404 or killed-mid-download means this
 *      fallback won't be available and SynthesizeAsync will error
 *      with a clear "no voice available" message pointing at
 *      setup-chatterbox.ps1 -IncludeDefaultVoice as an escape hatch.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxVoice
{
    GENERATED_BODY()

    /** Absolute or project-relative WAV path. 24 kHz mono, PCM int16
     *  or IEEE float32. See the struct-level comment for the exact
     *  contract. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FString WavFilePath;

    /** 24 kHz mono int16 PCM little-endian bytes. Byte count must be
     *  a multiple of 2. The subsystem converts to float32 internally
     *  before feeding speech_encoder. Ignored if WavFilePath is
     *  non-empty. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    TArray<uint8> ReferenceSamples;

    /** RESERVED for Phase E (precomputed voice conditioning). Leave
     *  empty in Phase D — setting it errors the synthesis call. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FString PrecomputedConditioningPath;
};

// ============================================================================
// Synthesis result
// ============================================================================

/**
 * Output of one successful SynthesizeAsync call — the PCM waveform
 * plus diagnostic timings. Mirrors FInoChatterboxRunner::FSynthesisResult,
 * re-expressed as a USTRUCT so Blueprints can read the timings / token
 * counts directly without a C++ wrapper.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxSynthesisResult
{
    GENERATED_BODY()

    /** Generated audio: 24 kHz mono int16 PCM little-endian bytes.
     *  Byte count is always a multiple of 2 (one int16 sample per
     *  two bytes). Always non-empty on success.
     *
     *  This is the "lingua franca" format for UE audio: feed it
     *  straight into USoundWaveProcedural::QueueAudio, into
     *  RuntimeAudioImporter's UStreamingSoundWave::AppendAudioDataFromRAW
     *  with ERuntimeRAWAudioFormat::Int16, save it verbatim as the
     *  data chunk of a WAV file (24 kHz mono), or send it over the
     *  network unmodified.
     *
     *  To get the sample count: AudioSamples.Num() / 2
     *  To get seconds:          use DurationSeconds below. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    TArray<uint8> AudioSamples;

    /** Always 24000 for Chatterbox Turbo. Included so callers passing
     *  AudioSamples to an audio pipeline don't have to hardcode the rate. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    int32 SampleRate = 24000;

    /** Duration of AudioSamples in seconds, pre-computed by the
     *  subsystem. Cheaper / more ergonomic than dividing
     *  AudioSamples.Num()/2 by SampleRate in every Blueprint. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float DurationSeconds = 0.0f;

    /** How many speech tokens the AR loop produced. Excludes the
     *  leading START and (if present) trailing STOP markers. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    int32 NumGeneratedTokens = 0;

    /** True if the loop terminated on the STOP token (normal); false
     *  if it hit MaxNewTokens (utterance may be truncated). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    bool bHitStopToken = false;

    /** Wall-clock time from start of SynthesizeAsync's worker dispatch
     *  to OnComplete being fired on the game thread. */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float TotalElapsedMs = 0.0f;

    /** Per-stage diagnostics (speech_encoder). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float EncoderMs = 0.0f;

    /** Per-stage diagnostics (embed_tokens across the whole loop). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float EmbedTotalMs = 0.0f;

    /** Per-stage diagnostics (language_model AR loop). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float LanguageModelMs = 0.0f;

    /** Per-stage diagnostics (conditional_decoder). */
    UPROPERTY(BlueprintReadOnly, Category = "InoAgents|Chatterbox")
    float DecoderMs = 0.0f;
};

// ============================================================================
// Project Settings — model registry
// ============================================================================

/**
 * One entry in the Chatterbox model registry (Project Settings →
 * Plugins → InoAgents → Chatterbox → Models). Describes where to
 * download a variant's files from when they are missing from disk.
 *
 * URL composition matches Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1:
 *
 *   <HuggingFaceRepoUrl>/resolve/<Revision>/onnx/<component>_<variant>.onnx
 *   <HuggingFaceRepoUrl>/resolve/<Revision>/onnx/<component>_<variant>.onnx_data
 *   <HuggingFaceRepoUrl>/resolve/<Revision>/tokenizer.json
 *   <HuggingFaceRepoUrl>/resolve/<Revision>/config.json
 *   <HuggingFaceRepoUrl>/resolve/<Revision>/generation_config.json
 *
 * where <component> is one of {speech_encoder, embed_tokens,
 * language_model, conditional_decoder}.
 *
 * TODO(Phase E): add per-file SHA-256 verification once canonical
 * hashes are available. The plugin already ships ComputeFileSha256 in
 * InoSha256.h (used by the LiteRT-LM subsystem); a forward-compatible
 * extension here will be a TMap<FString, FString> keyed by relative
 * filename.
 */
USTRUCT(BlueprintType)
struct FInoChatterboxModelEntry
{
    GENERATED_BODY()

    /** Human-readable name for the editor. Purely cosmetic. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FString DisplayName;

    /** Which quantization variant this entry describes. The subsystem
     *  matches on this (not on DisplayName) when resolving downloads. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    EInoChatterboxVariant Variant = EInoChatterboxVariant::Q4F16;

    /** HuggingFace repo root, e.g.
     *  "https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX". No
     *  trailing slash. The subsystem appends "/resolve/<rev>/..." per
     *  file. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FString HuggingFaceRepoUrl = TEXT("https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX");

    /** Git-style revision to pull — a commit hash for reproducible
     *  builds, or "main" during development. Matches the <rev> field
     *  in Plugins/InoAgents/Chatterbox/CHATTERBOX_VERSION. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Chatterbox")
    FString Revision = TEXT("main");
};

// ============================================================================
// Delegates
// ============================================================================
//
// All Chatterbox-facing delegates are dynamic (Blueprint-visible).
// Single-cast for the terminal events (OnLoaded / OnComplete) mirrors
// the LiteRT-LM pattern — one operation, one handler. Multicast for
// progress since it's useful to have UI + logger both bound.
// ============================================================================

/**
 * Fired once by UInoChatterboxTtsSubsystem::LoadModelsAsync when the
 * load finishes (successfully or not). On failure, ErrorMessage is
 * a human-readable summary suitable for logging or showing to the user.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnInoChatterboxModelsLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * Fired once by UInoChatterboxTtsSubsystem::SynthesizeAsync per call.
 * On success, bSuccess is true, Result.AudioSamples is the 24 kHz mono
 * int16 PCM LE byte buffer (see FInoChatterboxSynthesisResult), and
 * ErrorMessage is empty. On failure, bSuccess is false, Result is
 * default-initialized (AudioSamples empty), and ErrorMessage describes
 * what went wrong.
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnInoChatterboxSynthesisComplete,
    bool, bSuccess,
    FInoChatterboxSynthesisResult, Result,
    FString, ErrorMessage);

/**
 * Fired during multi-file model downloads.
 *
 *   Percent        — aggregate across every file in the queue (0..100).
 *   BytesReceived  — rolling sum of bytes written across all files so far.
 *   TotalBytes     — aggregate total, or -1 if any file's Content-Length
 *                    was missing (HF's 302 redirects occasionally strip it).
 *   bCompleted     — false for every intermediate progress tick; true on
 *                    exactly ONE terminal broadcast, fired after EVERY
 *                    file in the queue has been downloaded + atomic-
 *                    renamed on disk, BEFORE the subsystem chains into
 *                    its ThreadPool load. Bind this to flip UI state
 *                    from "downloading" to "loading" immediately,
 *                    without waiting for OnLoaded (the models still
 *                    have to construct — typically another 1-5 s).
 *                    On download failure this broadcast is NOT fired —
 *                    the error flows through OnLoaded(false, error).
 *
 * Declared separately from the LiteRT-LM delegate of the same shape
 * to keep the Chatterbox feature self-contained — Blueprint graphs do
 * not cross-pollinate LiteRT-LM and Chatterbox types.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnInoChatterboxDownloadProgress,
    float, Percent,
    int64, BytesReceived,
    int64, TotalBytes,
    bool,  bCompleted);

/**
 * Fired by UInoChatterboxTtsSubsystem's streaming synthesis path
 * (UInoChatterboxStreamSynthesize async action) for every incremental
 * audio chunk produced during synthesis.
 *
 *   AudioChunk       — NEW bytes since the last chunk, 24 kHz mono int16
 *                      PCM little-endian. Append directly to your player's
 *                      streaming buffer; do not re-concatenate anything
 *                      already delivered by a prior chunk.
 *   bIsFinal         — true exactly once, on the last chunk. After the
 *                      bIsFinal=true broadcast fires, no more chunks
 *                      will follow for this synthesis request.
 *   NumGeneratedTokens — running count of speech tokens the AR loop has
 *                      produced so far. Useful for progress UI
 *                      ("generated 180 of ~256 tokens...").
 *
 * Fires on the GAME THREAD — handlers can touch UObject state safely.
 * The runner produces these chunks on its worker thread; the subsystem
 * marshals each one via AsyncTask(GameThread).
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnInoChatterboxAudioChunk,
    const TArray<uint8>&, AudioChunk,
    bool,                 bIsFinal,
    int32,                NumGeneratedTokens);
