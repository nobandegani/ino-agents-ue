// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoElevenLabsTypes.generated.h"

/**
 * Output audio format for ElevenLabs streaming endpoints.
 *
 * Maps 1:1 to the `output_format` query parameter strings ElevenLabs
 * accepts. Only the subset we actually expose to Blueprint is listed here;
 * extend when a caller needs a different sample rate or codec.
 *
 * Pro-tier / Creator-tier notes mirror what the docs say at
 * https://elevenlabs.io/docs/api-reference/text-to-dialogue/stream — PCM
 * 44.1 kHz and MP3 192 kbps require paid account tiers.
 */
UENUM(BlueprintType)
enum class EInoElevenLabsOutputFormat : uint8
{
    Mp3_44100_128   UMETA(DisplayName = "MP3 44.1 kHz / 128 kbps"),
    Mp3_44100_64    UMETA(DisplayName = "MP3 44.1 kHz / 64 kbps"),
    Mp3_22050_32    UMETA(DisplayName = "MP3 22.05 kHz / 32 kbps"),
    Pcm_16000       UMETA(DisplayName = "PCM 16 kHz"),
    Pcm_24000       UMETA(DisplayName = "PCM 24 kHz"),
    Pcm_44100       UMETA(DisplayName = "PCM 44.1 kHz (Pro tier)"),
    Ulaw_8000       UMETA(DisplayName = "u-law 8 kHz"),
};

/** Maps to ElevenLabs' `apply_text_normalization` enum. */
UENUM(BlueprintType)
enum class EInoElevenLabsTextNormalization : uint8
{
    Auto,
    On,
    Off,
};

/**
 * One line of a dialogue request: a piece of text and the voice that
 * should speak it. ElevenLabs allows up to 10 unique voice IDs per
 * request; the async action validates that limit before issuing HTTP.
 */
USTRUCT(BlueprintType)
struct FInoElevenLabsDialogueInput
{
    GENERATED_BODY()

    /** Line of text to speak. Required. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    FString Text;

    /** ElevenLabs voice identifier. Required. Find yours at
     *  https://elevenlabs.io/app/voice-library. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    FString VoiceId;
};

/**
 * Full request payload for /v1/text-to-dialogue/stream.
 *
 * Optional fields left at their defaults are omitted from the generated
 * JSON body so ElevenLabs' own defaults take effect. In particular:
 *   - Empty ModelId       -> subsystem default (usually "eleven_v3").
 *   - Empty LanguageCode  -> omit (auto-detect).
 *   - Seed < 0            -> omit (non-deterministic sampling).
 */
USTRUCT(BlueprintType)
struct FInoElevenLabsDialogueRequest
{
    GENERATED_BODY()

    /** Dialogue lines in order. 1..10 unique voice IDs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    TArray<FInoElevenLabsDialogueInput> Inputs;

    /** ElevenLabs model id (e.g. "eleven_v3"). Empty = subsystem default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    FString ModelId;

    /** Audio codec / sample rate to request from ElevenLabs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    EInoElevenLabsOutputFormat OutputFormat = EInoElevenLabsOutputFormat::Mp3_44100_128;

    /** ISO 639-1 language code (e.g. "en", "es"). Empty = auto. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    FString LanguageCode;

    /** Voice settings - stability. Clamped 0..1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs",
              meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Stability = 0.5f;

    /** Deterministic sampling seed. Negative = omit. Max 4294967295. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    int64 Seed = -1;

    /** Text normalization mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|ElevenLabs")
    EInoElevenLabsTextNormalization ApplyTextNormalization = EInoElevenLabsTextNormalization::Auto;
};

// ---------------------------------------------------------------------------
// Delegates - dynamic multicast so UBlueprintAsyncActionBase can expose them
// as output exec pins.
//
// Parameter-passing convention:
//   - FString / enum / int64 / float / POD structs -> BY VALUE, matching
//     the LiteRtLm plugin convention (see InoLiteRtLmConversation
//     ToolTest.cpp:126, 146, 166, 206).
//   - TArray<T> and other containers -> `const TArray<T>&` (by const ref).
//     Passing TArray<uint8> by value here produces a cryptic Blueprint-time
//     "function/event does not match the necessary signature" error when
//     a user drags the latent node into a graph, because UHT generates
//     the Blueprint-side event handler with const& for containers but
//     tries to match it against a by-value declared delegate. The error
//     surfaces only at Blueprint compile, not at C++ compile - so it was
//     missed until the first in-editor test.
// ---------------------------------------------------------------------------

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FOnInoElevenLabsDialogueChunk,
    const TArray<uint8>&, AudioBytes,
    int64,                TotalBytesReceived);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FOnInoElevenLabsDialogueComplete,
    const TArray<uint8>&,    FullAudioBytes,
    EInoElevenLabsOutputFormat, OutputFormat);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnInoElevenLabsDialogueError,
    FString, ErrorMessage);
