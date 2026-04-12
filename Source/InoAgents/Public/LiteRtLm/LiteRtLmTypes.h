// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "LiteRtLmTypes.generated.h"

/**
 * Which inference backend the LiteRT-LM engine uses. Maps to the
 * `backend_str` argument of litert_lm_engine_settings_create().
 */
UENUM(BlueprintType)
enum class ELiteRtLmBackend : uint8
{
    Cpu  UMETA(DisplayName="CPU"),
    Gpu  UMETA(DisplayName="GPU (D3D12 via WebGPU accelerator on Windows)"),
};

/**
 * Convert ELiteRtLmBackend to the C string LiteRT-LM expects. The returned
 * pointer is a static string literal — do not free it, do not copy it, its
 * lifetime is the module's lifetime.
 */
INOAGENTS_API const char* LiteRtLmBackendToString(ELiteRtLmBackend Backend);

// ============================================================================
// Sampler + activation enums/structs
// ============================================================================

/** Sampling strategy for token selection. */
UENUM(BlueprintType)
enum class ELiteRtLmSamplerType : uint8
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
struct FLiteRtLmSamplerConfig
{
    GENERATED_BODY()

    /** Sampling strategy. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    ELiteRtLmSamplerType Type = ELiteRtLmSamplerType::TopK;

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
enum class ELiteRtLmActivationType : uint8
{
    F32  UMETA(DisplayName = "Float32 (full precision)"),
    F16  UMETA(DisplayName = "Float16 (half precision)"),
    I16  UMETA(DisplayName = "Int16"),
    I8   UMETA(DisplayName = "Int8 (most quantized)"),
};

// ============================================================================
// Model config
// ============================================================================

/**
 * Configuration for a LiteRT-LM model. Plain struct — no data asset
 * needed. Set the fields directly on the agent component or pass to
 * ULiteRtLmSubsystem::LoadModelAsync.
 */
USTRUCT(BlueprintType)
struct FLiteRtLmModelConfig
{
    GENERATED_BODY()

    // ----- Model file + backend -----

    /** Filename of the .litertlm model file. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString ModelFileName = TEXT("gemma-4-E4B-it.litertlm");

    /** Which backend the engine should use. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    ELiteRtLmBackend Backend = ELiteRtLmBackend::Cpu;

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
    FLiteRtLmSamplerConfig Sampler;

    /** Max tokens per response. 0 = unlimited. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation",
              meta = (ClampMin = "0"))
    int32 MaxOutputTokens = 0;

    /** Pre-populated conversation history as a JSON array. Empty = none.
     *  Format: [{"role":"user","content":"..."},{"role":"assistant","content":"..."}]
     *  Useful for resuming saved conversations or seeding backstory. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Conversation",
              meta = (MultiLine = true))
    FString InitialMessages;

    // ----- Engine optimization -----

    /** Activation precision. Lower = faster + less RAM.
     *
     *  NOTE (LiteRT-LM v0.10.1): non-F32 activation types cause
     *  send_message_stream to return error 13 even though the engine
     *  loads successfully. Default is F32 until a future LiteRT-LM
     *  release fixes this. The engine-level setting IS applied (the
     *  model file loads), but the conversation streaming path fails. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM|Engine")
    ELiteRtLmActivationType ActivationType = ELiteRtLmActivationType::F32;

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
struct FLiteRtLmModelEntry
{
    GENERATED_BODY()

    /** Human-readable name (for editor display). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString DisplayName;

    /** Filename on disk (must match FLiteRtLmModelConfig::ModelFileName). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString ModelFileName;

    /** Direct download URL. For Hugging Face:
     *  https://huggingface.co/<org>/<repo>/resolve/main/<file> */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|LiteRT-LM")
    FString DownloadUrl;
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
// All five delegates the Milestone D API exposes are declared here upfront,
// even though only FOnLiteRtLmModelLoaded is used by the D.1 sub-milestone.
// Centralizing them avoids header churn across sub-milestones and makes the
// full Blueprint API surface discoverable in one place.
//
// Dynamic delegates are Blueprint-visible but require binding via UFUNCTION-
// flagged methods on UObjects. Non-dynamic delegates support BindLambda but
// are not Blueprint-visible. We use dynamic delegates throughout because the
// API's primary audience is Blueprint designers.
// ============================================================================

/**
 * Fired once by ULiteRtLmSubsystem::LoadModelAsync when the load completes
 * (successfully or otherwise). Single-cast: one LoadModelAsync call attaches
 * one handler; there is no multicast model-loaded event.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnLiteRtLmModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * Fired per streaming chunk from ULiteRtLmConversation::SendMessageAsync.
 * Chunks are delivered on the game thread via AsyncTask. Blueprint code
 * typically binds a UMG text widget to this and appends chunks in real time.
 * Multicast so multiple observers (UI + logger + metrics panel, etc.) can
 * all watch.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmToken,
    FString, Chunk);

/**
 * Fired exactly once per successful SendMessageAsync call, after all tokens
 * have been delivered and any tool calls have been handled. FullText is the
 * concatenation of every Chunk broadcast during this send, with no trimming.
 * Either OnComplete or OnError fires per send, never both.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmComplete,
    FString, FullText);

/**
 * Fired exactly once per failed SendMessageAsync call. ErrorMessage describes
 * the failure in human-readable terms. Mutually exclusive with OnComplete.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmError,
    FString, ErrorMessage);

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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLiteRtLmSentence,
    FString, RawText,
    FString, CleanText);

/**
 * Fired at each newline boundary in the streaming token stream, right
 * after the corresponding OnSentence broadcast. Use this to insert a
 * timed pause between TTS audio segments via
 * UInoAgentsLiteRtLmDialogueQueue::EnqueuePause.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLiteRtLmNewLine);

/**
 * Diagnostic event fired after ULiteRtLmConversation has handled a tool call
 * end-to-end (looked up the tool in the subsystem's registry, invoked it on
 * the game thread, fed the result back into the LiteRT-LM conversation).
 *
 * Most Blueprint graphs do NOT need to bind this — the conversation handles
 * tool calls transparently. The delegate exists so debug UI can observe the
 * full tool-call round-trip.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnLiteRtLmToolCalled,
    FName, ToolName,
    FString, ArgumentsJson,
    FString, ResultJson);

/** Fired during model file download. Percent is 0..100 based on
 *  Content-Length. TotalBytes is -1 if the server didn't send
 *  Content-Length (rare for Hugging Face). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnInoAgentsModelDownloadProgress,
    float, Percent,
    int64, BytesReceived,
    int64, TotalBytes);
