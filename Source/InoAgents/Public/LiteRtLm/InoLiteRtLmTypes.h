// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "InoLiteRtLmTypes.generated.h"

/**
 * Which inference backend the LiteRT-LM engine uses. Maps to the
 * `backend_str` argument of litert_lm_engine_settings_create().
 */
UENUM(BlueprintType)
enum class EInoLiteRtLmBackend : uint8
{
    Cpu  UMETA(DisplayName="CPU"),
    Gpu  UMETA(DisplayName="GPU (D3D12 via WebGPU on Windows)"),
};

/**
 * Convert EInoLiteRtLmBackend to the C string LiteRT-LM expects. The returned
 * pointer is a static string literal — do not free it, do not copy it, its
 * lifetime is the module's lifetime.
 */
INOAGENTS_API const char* LiteRtLmBackendToString(EInoLiteRtLmBackend Backend);

// ============================================================================
// Sampler + activation enums/structs
// ============================================================================

/** Sampling strategy for token selection. */
UENUM(BlueprintType)
enum class EInoLiteRtLmSamplerType : uint8
{
    /** Probabilistically pick among the top-k tokens. */
    TopK    UMETA(DisplayName = "Top-K"),
    /** Top-k first, then pick among tokens summing to >= p probability. */
    TopP    UMETA(DisplayName = "Top-P"),
    /** Always pick the highest-probability token (deterministic). */
    Greedy  UMETA(DisplayName = "Greedy (deterministic)"),
};

/** Sampling parameters for token generation. */
USTRUCT(BlueprintType)
struct FInoLiteRtLmSamplerConfig
{
    GENERATED_BODY()

    /** Sampling strategy. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    EInoLiteRtLmSamplerType Type = EInoLiteRtLmSamplerType::TopK;

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

    /** Filename of the .litertlm model file. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString ModelFileName = TEXT("gemma-4-E4B-it.litertlm");

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

    // ----- Engine optimization -----

    /** Activation precision. Lower = faster + less RAM.
     *  F16 halves memory vs F32 with minimal quality loss on Gemma 4. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    EInoLiteRtLmActivationType ActivationType = EInoLiteRtLmActivationType::F16;

    /** Custom XNNPACK cache directory. Empty = default (next to model file). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    FString CacheDir;
};

/**
 * One entry in the model registry (Project Settings → Plugins →
 * InoAgents LiteRT-LM → Models). Maps a model filename to a download
 * URL so the agent component can auto-download on first use.
 */
USTRUCT(BlueprintType)
struct FInoLiteRtLmModelEntry
{
    GENERATED_BODY()

    /** Human-readable name (for editor display). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString DisplayName;

    /** Filename on disk (must match FInoLiteRtLmModelConfig::ModelFileName). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString ModelFileName;

    /** Direct download URL. For Hugging Face:
     *  https://huggingface.co/<org>/<repo>/resolve/main/<file> */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString DownloadUrl;

    /** Optional lowercase 64-character SHA-256 hex digest of the model file.
     *  Same format as `sha256sum` / Hugging Face LFS OIDs.
     *
     *  When set, UInoLiteRtLmSubsystem::LoadModelAsync verifies the on-disk
     *  file against this digest before handing it to the native engine.
     *  A mismatch for a locally-cached file triggers an automatic delete +
     *  re-download; a mismatch immediately after a fresh download is a hard
     *  failure (no infinite loop).
     *
     *  Leave empty to skip verification (back-compat with pre-SHA workflows).
     *  Obtain the hash from the model's upstream host — for Hugging Face
     *  LFS files the SHA-256 is the `oid` shown on the file's page or via
     *  the API's `X-Linked-Etag` header. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM",
              meta = (DisplayName = "Expected SHA-256"))
    FString ExpectedSha256;
};

/** Resolve the on-disk path for a model filename. Checks:
 *  1. PersistentDownloadDir/InoAgents/Models/ (downloaded/cached)
 *  2. Plugins/InoAgents/Models/ (legacy dev path)
 *  Returns empty string if not found anywhere. */
INOAGENTS_API FString LiteRtLmResolveModelPath(const FString& ModelFileName);

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

/** Fired during model file download.
 *
 *   Percent         — 0..100 based on Content-Length; 0 if TotalBytes<0.
 *   BytesReceived   — rolling sum of bytes written to disk so far.
 *   TotalBytes      — known total, or -1 if the server didn't send
 *                     Content-Length (rare for Hugging Face).
 *   bCompleted      — false for every intermediate progress tick;
 *                     true on exactly ONE terminal broadcast, fired
 *                     after the download has been fully written and
 *                     renamed on disk, BEFORE LoadModelAsync chains
 *                     into the ThreadPool load. Bind this to flip UI
 *                     state from "downloading" to "loading" without
 *                     waiting for OnLoaded (the model isn't usable
 *                     yet at this point — the engine still has to
 *                     construct). On download failure, bCompleted=true
 *                     is NOT fired; the error flows through OnLoaded
 *                     with bSuccess=false instead.
 */
DECLARE_DYNAMIC_DELEGATE_FourParams(FOnInoModelDownloadProgress,
    float, Percent,
    int64, BytesReceived,
    int64, TotalBytes,
    bool,  bCompleted);
