// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoNeuTtsNanoNativeTypes.generated.h"

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
enum class EInoNeuTtsNanoNativeBackboneVariant : uint8
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
 * UInoAgentsSettings::NeuTtsNanoNativeModels holds one of these per variant.
 * Seeded in the UInoAgentsSettings constructor with the Q4 defaults.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsNanoNativeModelEntry
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    FString DisplayName = TEXT("NeuTTS Nano Q4");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoNativeBackboneVariant BackboneVariant = EInoNeuTtsNanoNativeBackboneVariant::Q4;

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
 * Advanced tuning knobs for NeuTTS Nano load. Covers BOTH runtimes the
 * subsystem consumes:
 *
 *   - the llama.cpp backbone (GGUF LLM that emits FSQ speech tokens),
 *   - the NeuCodec ONNX decoder (FSQ ids → 24 kHz waveform).
 *
 * Default-constructed gives a safe, portable configuration:
 *   - CPU-only on both runtimes on Windows (DirectML opt-in to avoid
 *     surprise driver-bug exposure on unknown GPUs)
 *   - XNNPACK on Android decoder (matches the hardcoded pre-refactor
 *     default)
 *   - llama.cpp auto-threading (reasonable for most CPUs, occasionally
 *     over-subscribes hybrid cores — set LlmThreadCount to P-core
 *     count for consistent throughput)
 *
 * None of these fields have to be touched to get a working load; they
 * exist so you can opt into accelerators and pin thread pools when
 * benchmarking shows the defaults leave performance on the table.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsNanoNativePerformanceOptions
{
    GENERATED_BODY()

    // ========================================================================
    // llama.cpp backbone (stage 1: text prompt → FSQ speech tokens)
    // ========================================================================

    /** Threads for token-generation phase of the AR loop (passed to
     *  llama_context_params::n_threads). 0 = let llama.cpp auto-pick
     *  (usually one per logical core — can over-subscribe hybrid CPUs
     *  like Arrow Lake / Raptor Lake where E-cores set the pace of the
     *  slowest thread).
     *
     *  Good starting points on CPU-only inference:
     *    0   = auto (works on laptops / unknown hardware)
     *    4   = older laptops, thin-and-light configs
     *    8   = most modern Intel / AMD desktops
     *    P-core count of your hybrid CPU for best throughput.
     *
     *  Ignored when NumGpuLayers covers every transformer block (all
     *  generation work lives on the GPU; CPU only handles dispatch). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|LLM",
              meta = (ClampMin = "0", ClampMax = "128"))
    int32 LlmThreadCount = 0;

    /** Threads for the prompt-processing batch decode (passed to
     *  llama_context_params::n_threads_batch). 0 = inherit from the
     *  CtxParams default (typically same as n_threads).
     *
     *  Prompt prefill parallelises across all prompt tokens at once, so
     *  it can saturate more cores than per-token generation. If your
     *  desktop has 16+ cores but LlmThreadCount is pinned low to avoid
     *  per-token overhead, raising this separately can cut prefill
     *  latency in half. For NeuTTS prompts (typically a few hundred
     *  tokens including the reference-voice FSQ codes) the win is in
     *  the sub-second range but still worthwhile. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|LLM",
              meta = (ClampMin = "0", ClampMax = "128"))
    int32 LlmBatchThreadCount = 0;

    /** Flash-attention kernel policy for the attention layer.
     *    true  = LLAMA_FLASH_ATTN_TYPE_AUTO (llama.cpp decides per-
     *            backend whether the current device has a usable
     *            flash-attn kernel — safe default).
     *    false = LLAMA_FLASH_ATTN_TYPE_DISABLED (force the standard
     *            attention path — useful only when diagnosing
     *            suspected flash-attn numerical issues).
     *
     *  For NeuTTS's short contexts (a few hundred tokens) the perf win
     *  is small, but memory footprint shrinks noticeably, which helps
     *  on memory-pressured Android devices. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|LLM")
    bool bFlashAttention = true;

    /** Memory-map the GGUF instead of reading it into heap (passed to
     *  llama_model_params::use_mmap). Almost always leave on — mmap
     *  gives near-zero load time and shared-COW page caching. Turn off
     *  only for platforms where mmap misbehaves (rare) or for
     *  debugging a file-IO issue. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|LLM")
    bool bUseMmap = true;

    /** Lock model pages in RAM (passed to llama_model_params::use_mlock).
     *  Off by default. Turn on if you're seeing paging hiccups during
     *  inference on a low-RAM system — locks the ~195 MB GGUF into
     *  physical memory so other processes can't swap it out. May fail
     *  silently on platforms without mlock privileges (Android without
     *  root, sandboxed iOS). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|LLM")
    bool bUseMlock = false;

    // ========================================================================
    // NeuCodec ONNX decoder (stage 2: FSQ speech tokens → 24 kHz audio)
    // ========================================================================

    /** ORT intra-op threads (parallelism INSIDE one op — matmul, conv
     *  SIMD, etc.) for the decoder session. 0 = ORT default (typically
     *  one per physical core). Same hybrid-CPU caveat as LlmThreadCount:
     *  pin to P-core count for consistent throughput on Arrow Lake /
     *  Raptor Lake. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Decoder",
              meta = (ClampMin = "0", ClampMax = "128"))
    int32 DecoderIntraOpThreadCount = 0;

    /** ORT inter-op threads (parallelism BETWEEN different ops in the
     *  same inference). 0 = ORT default, 1 = fully sequential. The
     *  NeuCodec decoder is a mostly-sequential graph; 1 is usually
     *  optimal and adding threads just adds scheduling noise. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Decoder",
              meta = (ClampMin = "0", ClampMax = "16"))
    int32 DecoderInterOpThreadCount = 1;

    // ---- Windows: DirectML ----

    /** Use DirectML for the NeuCodec decoder on Windows. Registers
     *  providers [DirectMl, Cpu] so DML takes any op it has a kernel
     *  for, CPU picks up the rest. Ignored on non-Windows.
     *
     *  Typical speedups vs. CPU-only on modern hardware:
     *    - Intel Arc iGPU (Xe-LPG on Core Ultra 200):  2-4×
     *    - NVIDIA RTX 3060+:                           5-10×
     *    - AMD RX 6000+:                               4-6×
     *
     *  The NeuCodec decoder is a straightforward 1D-conv pipeline, the
     *  same structural class that DML handles correctly on Chatterbox's
     *  conditional_decoder. First-run has a shader-compile hitch (one
     *  time per session); subsequent runs are fully accelerated.
     *
     *  Default false — opt in after verifying on your target GPU /
     *  adapter combination. DirectML registration silently falls back
     *  to CPU if the adapter isn't D3D12-capable, so flipping this on
     *  can't make things worse; it just may or may not make them
     *  faster depending on the hardware. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Decoder|DirectML")
    bool bPreferDirectMl = false;

    /** Windows-only: D3D12 adapter index for DirectML, matching
     *  IDXGIFactory::EnumAdapters order. 0 = default adapter
     *  (primary display GPU — integrated on laptops, discrete on
     *  typical desktops with a dGPU).
     *
     *  Use the Ino.Onnx.ListDmlAdaptersTest console command to
     *  enumerate adapters on the current machine before picking.
     *  Ignored when bPreferDirectMl is false or on non-Windows. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Decoder|DirectML",
              meta = (ClampMin = "0", ClampMax = "16"))
    int32 DirectMlAdapterIndex = 0;

    // ---- Android: XNNPACK ----

    /** Use XNNPACK for the NeuCodec decoder on Android. Registers
     *  providers [Xnnpack, Cpu] — XNNPACK takes the ARM-NEON-friendly
     *  ops (conv, matmul, gemm), CPU picks up the rest.
     *
     *  XNNPACK is purpose-built for ARM mobile CPUs and is usually a
     *  net win on the NeuCodec decoder. Safe default; flip to false
     *  only if benchmarks on your target device show CPU alone is
     *  actually faster. Ignored on non-Android platforms. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Decoder|Android")
    bool bUseXnnpack = true;

    // ---- Diagnostics (off in production) ----

    /** Write ORT's per-op profiler output (onnxruntime_profile_*.json
     *  next to the executable) for the decoder session. ~5-10 %
     *  runtime overhead — off by default. Load the output in
     *  chrome://tracing or Perfetto to see exactly which op dominates
     *  a specific decode call. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Diagnostics")
    bool bEnableOrtProfiling = false;

    /** Raise ORT's log severity to VERBOSE (0) for the decoder session.
     *  Off by default (ORT default is WARNING = 2). Useful when
     *  diagnosing DML kernel-support fallbacks, provider-dispatch
     *  decisions, or graph-transformer rewrites — ORT's verbose log
     *  tells you which DML_OPERATOR_*_DESC validation failed instead
     *  of a generic E_INVALIDARG line. ~5 % overhead plus a very chatty
     *  output log; turn on only for specific debug runs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Performance|Diagnostics")
    bool bEnableVerboseOrtLogging = false;
};

/**
 * Runtime-side load configuration. Chooses a variant; the URLs / file
 * names / SHA come from the matching UInoAgentsSettings entry.
 *
 * Blueprint consumers build this with a single enum pick; everything
 * else has sensible defaults.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsNanoNativeModelConfig
{
    GENERATED_BODY()

    /** Which variant to load. Matched against UInoAgentsSettings::FindNeuTtsNanoNativeModel. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoNativeBackboneVariant Variant = EInoNeuTtsNanoNativeBackboneVariant::Q4;

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

    /** Advanced perf tuning — thread counts, accelerator selection,
     *  diagnostics. All fields have safe defaults; most callers leave
     *  this default-constructed. See FInoNeuTtsNanoNativePerformanceOptions
     *  for the per-field discussion. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano")
    FInoNeuTtsNanoNativePerformanceOptions Performance;
};

/**
 * Per-synthesis options. Defaults match the official Python pipeline
 * (neutts/neutts.py :: _infer_ggml: temperature=1.0, top_k=50,
 * max_tokens=max_context).
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsNanoNativeSynthesisOptions
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

    /** Nucleus (top-p) sampling cutoff. NeuTTS's GGUF path inherits
     *  llama-cpp-python's default of 0.95 — a low-probability-tail cut
     *  that keeps generation focused. Set to 1.0 to disable. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "0.1", ClampMax = "1.0"))
    float TopP = 0.95f;

    /** Minimum-p sampling cutoff. NeuTTS's GGUF path inherits
     *  llama-cpp-python's default of 0.05 — drops tokens whose
     *  probability is below min_p × (prob of the most likely token).
     *  Set to 0.0 to disable. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinP = 0.05f;

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

    /** Streaming chunk size — how many NEW FSQ speech-ids must accumulate
     *  before the worker fires OnAudioChunk with a partial-waveform
     *  chunk. Only consulted by SynthesizeStreamAsync (ignored by the
     *  one-shot SynthesizeAsync path). Tradeoffs:
     *
     *    0      = disable streaming (fall back to one-shot semantics;
     *             OnAudioChunk fires exactly once with the full waveform
     *             and bIsFinal=true immediately before OnComplete)
     *    20-25  = low latency — one NeuCodec decoder call every ~0.4-0.5 s
     *             of audio (NeuCodec FSQ token rate is ~50 Hz), first
     *             chunk arrives in ~0.4-0.6 s wall-clock on a desktop CPU
     *    50-75  = moderate — one decoder call per ~1-1.5 s of audio
     *    100+   = approaching one-shot — only worth it if decoder cost
     *             dominates wall time
     *
     *  Every chunk re-runs the decoder on the FULL prefix of speech-ids
     *  so far (not just the delta). That means total decoder work
     *  scales roughly as O(N^2 / StreamChunkTokens) — a sentence that
     *  produces ~500 FSQ codes with StreamChunkTokens=25 does ~20
     *  decoder calls at avg length 250, about 10× the CPU work of a
     *  single one-shot decode. Worth it for conversational UX because
     *  the first-audio latency is divided by ~(N / StreamChunkTokens),
     *  but don't pick tiny chunk sizes "to be safe" — they multiply
     *  total wall-clock cost. 25 is a good default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|NeuTTS Nano|Streaming",
              meta = (ClampMin = "0", ClampMax = "512"))
    int32 StreamChunkTokens = 25;
};

/**
 * One file in the NeuTTS Nano download queue — internal detail of the
 * auto-download flow. Declared at namespace scope (not nested in
 * UInoNeuTtsNanoNativeSubsystem) so the build-queue helper in the subsystem's
 * .cpp anonymous namespace can reference it without friending.
 *
 * Mirrors FInoChatterboxDownloadFile — same semantics, different
 * containing subsystem. The duplication is flagged tech debt; a future
 * refactor will factor both into Private/InoHttpDownload/FDownloadFile.
 */
struct FInoNeuTtsNanoNativeDownloadFile
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
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnInoNeuTtsNanoNativeModelLoaded,
    bool,    bSuccess,
    FString, ErrorMessage);

/**
 * Fired exactly once when SynthesizeAsync (or SynthesizeStreamAsync)
 * resolves. On success, PcmInt16LE carries raw 24 kHz mono int16
 * little-endian PCM bytes (directly feedable into
 * UStreamingSoundWave::AppendAudioDataFromRAW via the RuntimeAudioImporter
 * plugin — matches Chatterbox's output contract).
 *
 * NOTE: no SampleRate param — NeuTTS Nano's output rate is fixed at
 * 24 kHz by the NeuCodec decoder and cannot be configured at runtime
 * (it would require retraining the codec). If you need the rate value
 * (e.g. to construct a USoundWaveProcedural or a WAV header), call
 * UInoNeuTtsNanoNativeSubsystem::GetOutputSampleRate() — a BlueprintPure
 * getter that returns 24000.
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnInoNeuTtsNanoNativeSynthesisComplete,
    bool,                   bSuccess,
    const TArray<uint8>&,   PcmInt16LE,
    FString,                ErrorMessage);
// Note: PcmInt16LE is `const TArray<uint8>&`, not TArray<uint8> by value.
// UE's BP reflection can't pass TArray-by-value through a dynamic delegate —
// it errors out with "No value will be returned by reference. Parameter
// 'PcmInt16LE'" when you try to bind a CustomEvent. Matches the pattern
// FOnInoChatterboxAudioChunk uses for the same AudioChunk byte buffer.

/**
 * Multicast download-progress signal.
 *
 *   Percent       — 0..100 (clamped).
 *   BytesReceived — on-disk bytes written across all files so far.
 *   TotalBytes    — aggregate, or -1 if any file's Content-Length was
 *                   missing (HF CDN sometimes strips it).
 *   bCompleted    — false for every intermediate progress tick; true on
 *                   exactly ONE terminal broadcast, fired after every
 *                   queued file has been atomic-renamed on disk, BEFORE
 *                   the subsystem chains into its ThreadPool load. Bind
 *                   this to flip UI state from "downloading" to
 *                   "loading" immediately, without waiting for OnLoaded
 *                   (the backbone + codec still have to construct —
 *                   typically another 1-3 s). On download failure this
 *                   broadcast is NOT fired — the error flows through
 *                   OnLoaded(false, err).
 */
DECLARE_DYNAMIC_DELEGATE_FourParams(FOnInoNeuTtsNanoNativeDownloadProgress,
    float, Percent,
    int64, BytesReceived,
    int64, TotalBytes,
    bool,  bCompleted);

/**
 * Fired by UInoNeuTtsNanoNativeSubsystem::SynthesizeStreamAsync for each
 * incremental audio delta produced during synthesis.
 *
 *   AudioChunk       — NEW bytes since the last chunk, 24 kHz mono int16
 *                      PCM little-endian. Append directly to your player's
 *                      streaming buffer; do NOT re-concatenate anything
 *                      already delivered by a prior chunk.
 *   bIsFinal         — true exactly once, on the last chunk. After
 *                      bIsFinal=true fires, no more chunks will follow
 *                      for this synthesis request; OnComplete follows
 *                      shortly after with the full concatenated waveform.
 *   NumSpeechIds     — running count of FSQ speech-ids the AR loop has
 *                      produced so far. Useful for progress UI
 *                      ("generated 180 speech tokens…").
 *
 * Fires on the GAME THREAD (marshalled via AsyncTask from the worker).
 * Handlers can touch UObject state safely.
 *
 * AudioChunk is `const TArray<uint8>&` (not by value) because UE's
 * Blueprint reflection layer cannot pass a TArray by value through a
 * dynamic delegate — the BindDynamic path fails with "No value will be
 * returned by reference. Parameter 'AudioChunk'". Same reason
 * FOnInoNeuTtsNanoNativeSynthesisComplete above uses a const ref; matches
 * Chatterbox's FOnInoChatterboxAudioChunk shape exactly.
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnInoNeuTtsNanoNativeAudioChunk,
    const TArray<uint8>&, AudioChunk,
    bool,                 bIsFinal,
    int32,                NumSpeechIds);

// ============================================================================
// Free-function helpers
// ============================================================================

/**
 * Returns "q4" / "q8" (or similar short tokens) for a variant. Used to
 * build variant-scoped paths and directory names.
 */
INONEUTTSNATIVE_API FString NeuTtsNanoNativeVariantToString(EInoNeuTtsNanoNativeBackboneVariant Variant);

/**
 * Absolute directory where model files for a given variant live:
 *   {PersistentDownloadDir}/InoAgents/Models/NeuTtsNanoNative/{variant}/
 *
 * Matches the layout UInoNeuTtsNanoNativeSubsystem uses as both its download
 * target and its model-load source. The shape mirrors Chatterbox's
 * per-variant directory pattern — makes a future multi-variant install
 * (Q4 + Q8 coexisting) trivial.
 */
INONEUTTSNATIVE_API FString NeuTtsNanoNativeResolveModelDir(EInoNeuTtsNanoNativeBackboneVariant Variant);
