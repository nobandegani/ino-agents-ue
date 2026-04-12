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
 * SentenceText is the accumulated line trimmed of whitespace — typically
 * a full sentence, a paragraph, or a numbered list item, depending on
 * how the model formats its response.
 *
 * Fires zero or more times per send, between OnToken broadcasts and before
 * the terminal OnComplete. The concatenation of every SentenceText plus
 * any trailing fragment (delivered only via OnComplete) equals the full
 * assistant response.
 *
 * Primary use case: pipe each line to ElevenLabs TTS independently so
 * audio generation starts before the full response is done.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLiteRtLmSentence,
    FString, SentenceText);

/**
 * Fired at each newline boundary in the streaming token stream, right
 * after the corresponding OnSentence broadcast. Use this to insert a
 * timed pause between TTS audio segments via
 * UInoAgentsTtsAudioQueue::EnqueuePause.
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
