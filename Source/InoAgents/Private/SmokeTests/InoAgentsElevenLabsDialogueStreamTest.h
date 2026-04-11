// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ElevenLabs/ElevenLabsTypes.h"

#include "InoAgentsElevenLabsDialogueStreamTest.generated.h"

class UElevenLabsTextToDialogueStream;

/**
 * One-shot observer for the InoAgents.ElevenLabs.DialogueStreamTest
 * console command. Parallel to the LiteRtLm ConversationToolTest
 * observer pattern - holds refs, binds dynamic delegates, writes the
 * resulting audio to disk, tears itself down in Finish().
 *
 * Delegate handlers take parameters BY VALUE (not const ref) to match
 * the plugin's bind-time convention. See
 * InoAgentsLiteRtLmConversationToolTest.cpp:126 for the rationale.
 */
UCLASS()
class UInoAgentsElevenLabsDialogueStreamTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime    = 0.0;
    int32  NumChunks    = 0;
    int64  TotalBytes   = 0;

    /** Resolved once in Finish() based on the request's output format. */
    FString OutputFilePath;

    UPROPERTY()
    TObjectPtr<UElevenLabsTextToDialogueStream> Action = nullptr;

    /** Remembered at dispatch time so Finish() can pick the right file
     *  extension (.mp3 / .pcm / .ulaw) without reparsing the request. */
    EElevenLabsOutputFormat OutputFormat = EElevenLabsOutputFormat::Mp3_44100_128;

    UFUNCTION()
    void HandleAudioChunk(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);

    UFUNCTION()
    void HandleComplete(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat Format);

    UFUNCTION()
    void HandleError(FString ErrorMessage);

private:
    void Finish();
};
