// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

// Pulls in FInoDownloadProgress for the download-progress delegate below.
// InoNodes is a PUBLIC dep of this module so consumers binding to the
// delegate get the struct definition for free.
#include "InoDownloader.h"

#include "InoLiteRtLmTypes.generated.h"

/**
 * Which inference backend the LiteRT-LM engine uses. Maps to the
 * `backend_str` argument of litert_lm_engine_settings_create().
 */
UENUM(BlueprintType)
enum class EInoLiteRtLmBackend : uint8
{
    Cpu  UMETA(DisplayName="CPU"),
    Gpu  UMETA(DisplayName="GPU"),
    Npu  UMETA(DisplayName="NPU"),
};

/**
 * Convert EInoLiteRtLmBackend to the C string LiteRT-LM expects. The returned
 * pointer is a static string literal — do not free it, do not copy it, its
 * lifetime is the module's lifetime.
 */
INOLITERTLM_API const char* LiteRtLmBackendToString(EInoLiteRtLmBackend Backend);

// ============================================================================
// Sampler + activation enums/structs
// ============================================================================

/**
 * Sampling strategy for token selection.
 *
 * IMPORTANT: the staged LiteRT-LM runtime's sampler factory implements
 * only Top-P on CPU/GPU (Top-K and a dedicated Greedy sampler are not
 * built in). When a per-conversation SessionConfig is attached
 * (FInoLiteRtLmModelConfig::bAttachSessionConfig), TopK / Greedy are
 * therefore clamped to TopP at session-config build time (with a
 * Warning) to avoid an UnimplementedError that would NULL the engine.
 * For deterministic output prefer Greedy (mapped to Top-P with
 * temperature 0) rather than relying on a dedicated greedy kernel.
 */
UENUM(BlueprintType)
enum class EInoLiteRtLmSamplerType : uint8
{
    /** Probabilistically pick among the top-k tokens. NOTE: clamped to
     *  Top-P in the current staged runtime (no CPU/GPU top-k kernel). */
    TopK    UMETA(DisplayName = "Top-K (clamped to Top-P)"),
    /** Top-k first, then pick among tokens summing to >= p probability.
     *  The only natively-supported probabilistic sampler. */
    TopP    UMETA(DisplayName = "Top-P"),
    /** Deterministic. Mapped to Top-P with temperature 0 (no dedicated
     *  greedy kernel in the staged runtime). */
    Greedy  UMETA(DisplayName = "Greedy (deterministic, via Top-P t=0)"),
};

/** Sampling parameters for token generation. */
USTRUCT(BlueprintType)
struct FInoLiteRtLmSamplerConfig
{
    GENERATED_BODY()

    /** Sampling strategy. Defaults to Top-P — the only sampler the
     *  staged CPU/GPU runtime implements. TopK/Greedy are accepted but
     *  clamped to Top-P (see EInoLiteRtLmSamplerType). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    EInoLiteRtLmSamplerType Type = EInoLiteRtLmSamplerType::TopP;

    /** Number of top tokens to consider (for TopK / TopP). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (ClampMin = "1"))
    int32 TopK = 40;

    /** Cumulative probability threshold (for TopP). 0..1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TopP = 0.95f;

    /** Temperature. 0 = greedy, <1 = focused, 1 = neutral, >1 = creative. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Temperature = 0.8f;

    /** RNG seed for reproducible sampling. <0 = non-deterministic. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    int32 Seed = -1;
};

/** Activation precision for inference. Lower = faster + less RAM but
 *  more quantization noise. */
UENUM(BlueprintType)
enum class EInoLiteRtLmActivationType : uint8
{
    F32  UMETA(DisplayName = "Float32 (full precision)"),
    F16  UMETA(DisplayName = "Float16 (half precision)"),
    I16  UMETA(DisplayName = "Int16"),
    I8   UMETA(DisplayName = "Int8 (most quantized)"),
};

// ============================================================================
// Conversation history
// ============================================================================

/** Role in a conversation message. */
UENUM(BlueprintType)
enum class EInoLiteRtLmMessageRole : uint8
{
    User       UMETA(DisplayName = "User"),
    Assistant  UMETA(DisplayName = "Assistant"),
};

/**
 * One message in a conversation history. Used for pre-populating
 * conversations with saved history or seeding backstory examples.
 */
USTRUCT(BlueprintType)
struct FInoLiteRtLmMessage
{
    GENERATED_BODY()

    /** Who sent this message. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    EInoLiteRtLmMessageRole Role = EInoLiteRtLmMessageRole::User;

    /** Message text content. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (MultiLine = true))
    FString Content;
};

// ============================================================================
// Model config
// ============================================================================

/**
 * Configuration for a LiteRT-LM model. Plain struct — no data asset
 * needed. Set the fields directly on the agent component or pass to
 * UInoLiteRtLmSubsystem::LoadModelAsync.
 */
USTRUCT(BlueprintType)
struct FInoLiteRtLmModelConfig
{
    GENERATED_BODY()

    // ----- Model file + backend -----

    /** Filename of the .litertlm model file. Empty = pick the first
     *  entry in Project Settings → Plugins → InoLiteRtLm → Models
     *  (matches the forgiving lookup pattern InoNeuTTS uses for its
     *  backbone/decoder names). Set to a DisplayName or LocalFileName
     *  from the registry to load a specific bundle. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString ModelFileName;

    /** Which backend the engine should use. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    EInoLiteRtLmBackend Backend = EInoLiteRtLmBackend::Cpu;

    /** Engine-level token budget (KV cache size). 0 = engine default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (ClampMin = "0"))
    int32 MaxNumTokens = 0;

    // ----- Conversation -----

    /** System prompt. Plain text — wrapped for the native API internally. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation",
              meta = (MultiLine = true))
    FString SystemMessage;

    /** Sampling parameters: temperature, top-k, top-p, seed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation")
    FInoLiteRtLmSamplerConfig Sampler;

    /** Max tokens per response. 0 = unlimited. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation",
              meta = (ClampMin = "0"))
    int32 MaxOutputTokens = 0;

    /** Attach a per-conversation LiteRtLmSessionConfig so Sampler + MaxOutputTokens
     *  above are actually applied. Default false because v0.11.0-rc.1 made
     *  Conversation::Create return NULL whenever a user SessionConfig was attached
     *  (Gemma 4); v0.11.0 final may or may not have fixed it — flip on to test.
     *  When false, conversations run with engine defaults and the two fields above
     *  are silently ignored. See InoLiteRtLmConversation.cpp for the regression
     *  history. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation",
              meta = (DisplayName = "Attach Session Config (experimental)"))
    bool bAttachSessionConfig = false;

    // ----- Engine optimization -----

    /** Activation precision. Lower = faster + less RAM.
     *  F16 halves memory vs F32 with minimal quality loss on Gemma 4. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    EInoLiteRtLmActivationType ActivationType = EInoLiteRtLmActivationType::F16;

    /** Custom XNNPACK cache directory. Empty = default (next to model file). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    FString CacheDir;

    /**
     * Run a throwaway prime generation right after the engine is created
     * (on the load worker thread, before OnLoaded fires). This pays the
     * one-time XNNPACK/GPU shader-compile + KV-cache allocation cost up
     * front so the user's FIRST real SendMessageAsync streams its first
     * token promptly instead of stalling for seconds with no feedback.
     *
     * Costs a few seconds of extra load time. Default false to preserve
     * the historical fast-OnLoaded behaviour; turn on for production /
     * GPU builds where first-token latency matters more than load time.
     * Warmup failure never fails the load — it logs a warning and
     * proceeds.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    bool bWarmUpOnLoad = false;
};

/**
 * One entry in the model registry (Project Settings → Plugins →
 * InoLiteRtLm → Models). Maps a display name + on-disk filename to a
 * download URL so the subsystem can auto-download on first use.
 *
 * The file lands at:
 *   <FPaths::ProjectPersistentDownloadDir()>/ino-agents/lite-rt-lm/<LocalFileName>
 *
 * If the file is already present at that path, no download happens
 * and DownloadUrl is unused. ExpectedSha256 (when set) is checked
 * after download AND against any existing cached file, so a corrupt
 * cached file is re-downloaded automatically.
 */
USTRUCT(BlueprintType)
struct FInoLiteRtLmModelEntry
{
    GENERATED_BODY()

    /**
     * Human-readable name shown in editor / Blueprint pickers.
     * Also the lookup key used by `FInoLiteRtLmModelConfig::ModelFileName`
     * via `UInoLiteRtLmSettings::FindModel` (case-insensitive — falls
     * back to LocalFileName match if nothing matches by display name).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DisplayName;

    /**
     * Direct download URL. For Hugging Face:
     *   https://huggingface.co/<org>/<repo>/resolve/main/<file>
     * Anything FHttpModule can GET works (HuggingFace, S3, your CDN).
     * Public URLs only — no auth handling.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DownloadUrl;

    /**
     * Filename to save as locally. The runtime concatenates this with
     * <persistent>/ino-agents/lite-rt-lm/ to get the full path. Must end in
     * `.litertlm` for LiteRT-LM to recognize it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString LocalFileName;

    /**
     * Hex-encoded SHA-256 hash for integrity verification. Optional
     * but strongly recommended. Empty = skip verification (download is
     * trusted by URL only). Lowercase, 64 chars, no separators —
     * same format as `sha256sum` / Hugging Face LFS OIDs.
     *
     * When set, the subsystem verifies the on-disk file against this
     * digest before handing it to the native engine. A mismatch for a
     * locally-cached file triggers an automatic delete + re-download;
     * a mismatch immediately after a fresh download is a hard failure
     * (no infinite loop).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model",
              meta = (DisplayName = "Expected SHA-256"))
    FString ExpectedSha256;

    /**
     * Total file size in bytes. Used for the download-progress
     * percentage when the server doesn't return Content-Length on
     * GET. 0 = probe via a HEAD request before downloading.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    int64 FileSizeBytes = 0;

    /**
     * IETF language tag the model was trained on / is intended for
     * ("en", "en-us", "ja", "multi"). Informational — drives no
     * runtime behavior in this module today, but lets UI / picker
     * code filter the list per locale.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString Language = TEXT("en");

    /**
     * Quantization label. Informational only — doesn't change runtime
     * behavior (the dtype is intrinsic to the .litertlm file).
     * Common values: "Q4_0", "Q8_0", "FP16", "INT8".
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString Quantization;
};

/** Resolve the on-disk path for a model filename. Checks:
 *  1. <ProjectPersistentDownloadDir>/ino-agents/lite-rt-lm/ (downloaded/
 *     cached; same path UInoLiteRtLmSettings::ResolveLocalPath builds)
 *  2. <InoAgents plugin base>/LiteRTLM/ (legacy dev path)
 *  Returns empty string if not found anywhere. */
INOLITERTLM_API FString LiteRtLmResolveModelPath(const FString& LocalFileName);

// ============================================================================
// Delegates
// ============================================================================
//
// All delegates are declared here upfront. Centralizing them avoids
// header churn and makes the full Blueprint API surface discoverable
// in one place.
//
// Dynamic delegates are Blueprint-visible but require binding via UFUNCTION-
// flagged methods on UObjects. Non-dynamic delegates support BindLambda but
// are not Blueprint-visible. We use dynamic delegates throughout because the
// API's primary audience is Blueprint designers.
// ============================================================================

/**
 * Fired once by UInoLiteRtLmSubsystem::LoadModelAsync when the load completes
 * (successfully or otherwise). Single-cast: one LoadModelAsync call attaches
 * one handler; there is no multicast model-loaded event.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnInoLiteRtLmModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * Fired per streaming chunk from UInoLiteRtLmConversation::SendMessageAsync.
 * Chunks are delivered on the game thread via AsyncTask. Blueprint code
 * typically binds a UMG text widget to this and appends chunks in real time.
 * Multicast so multiple observers (UI + logger + metrics panel, etc.) can
 * all watch.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoLiteRtLmToken,
    FString, RawText,
    FString, CleanText);

/**
 * Fired exactly once per successful SendMessageAsync call, after all tokens
 * have been delivered and any tool calls have been handled. FullText is the
 * concatenation of every Chunk broadcast during this send, with no trimming.
 * Either OnComplete or OnError fires per send, never both.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoLiteRtLmComplete,
    FString, FullText);

/**
 * Fired exactly once per failed SendMessageAsync call. ErrorMessage describes
 * the failure in human-readable terms. Mutually exclusive with OnComplete.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoLiteRtLmError,
    FString, ErrorMessage);

/**
 * Fired at the start of every SendMessageAsync call, on the game thread,
 * with the user's text BEFORE any context augmentation / history
 * recording is applied. Use for chat UI that wants to echo the user's
 * message as soon as it's submitted (without waiting for the model to
 * start generating) or for analytics / logging.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoLiteRtLmUserMessage,
    FString, UserText);

/**
 * Fired each time the streaming token buffer crosses a newline boundary.
 * Delivers two versions of the text:
 *
 *   RawText   — the line as the model produced it, including any
 *               [emotion] or [audio] tags (e.g. "[cheerfully] Hello!").
 *               Send this to ElevenLabs TTS — it consumes the tags as
 *               delivery instructions and does not speak them aloud.
 *
 *   CleanText — the same line with all [bracketed] tags stripped, for
 *               use in subtitles, chat bubbles, or any display that
 *               shouldn't show the raw tags (e.g. "Hello!").
 *
 * Fires zero or more times per send, between OnToken broadcasts and
 * before the terminal OnComplete.
 *
 * Primary use case: pipe RawText to ElevenLabs for expressive TTS,
 * display CleanText in the game's subtitle UI.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoLiteRtLmSentence,
    FString, RawText,
    FString, CleanText);

/**
 * Fired at every sentence-split boundary in the streaming token stream,
 * right after the corresponding OnSentence broadcast. The set of
 * boundaries that count is controlled by the conversation's
 * SentenceSplitFlags (see ELiteRtLmSentenceSplit) — newline alone, or
 * newline + period + comma + ..., or any subset.
 *
 * Use this for cues that fire per emitted sentence regardless of
 * payload — animation triggers, viseme resets, custom effects. The
 * dialogue queue handles silence between sentences itself via the
 * OnSentence flow, so there is no need to bind this just to add
 * pauses.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoLiteRtLmSentenceBoundary);

/**
 * Diagnostic event fired after UInoLiteRtLmConversation has handled a tool call
 * end-to-end (looked up the tool in the subsystem's registry, invoked it on
 * the game thread, fed the result back into the LiteRT-LM conversation).
 *
 * Most Blueprint graphs do NOT need to bind this — the conversation handles
 * tool calls transparently. The delegate exists so debug UI can observe the
 * full tool-call round-trip.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnInoLiteRtLmToolCalled,
    FName, ToolName,
    FString, ArgumentsJson,
    FString, ResultJson);

/**
 * Single-cast delegate fired during model file download. Re-uses
 * InoNodes' shared `FInoDownloadProgress` payload so callers see the
 * same struct everywhere (BytesReceived, TotalBytes, ProgressPercent,
 * BytesPerSecond, EstimatedSecondsRemaining, RetryAttempt, ...).
 *
 * Always fires on the game thread. If the model is already cached on
 * disk this delegate never fires — the load proceeds straight to
 * SHA verification and engine construction.
 */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoLiteRtLmDownloadProgressDelegate,
    const FInoDownloadProgress&, Progress);
