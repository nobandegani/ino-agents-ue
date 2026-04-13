// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ElevenLabs/InoElevenLabsTypes.h"

#include "InoElevenLabsDialogueStreamTest.generated.h"

class UInoElevenLabsTextToDialogueStream;

/**
 * One-shot observer for the Ino.ElevenLabs.DialogueStreamTest
 * console command. Parallel to the LiteRtLm ConversationToolTest
 * observer pattern - holds refs, binds dynamic delegates, writes the
 * resulting audio to disk, tears itself down in Finish().
 *
 * Delegate handlers take parameters BY VALUE (not const ref) to match
 * the plugin's bind-time convention. See
 * InoLiteRtLmConversationToolTest.cpp:126 for the rationale.
 */
UCLASS()
class UInoElevenLabsDialogueStreamTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime    = 0.0;
    int32  NumChunks    = 0;
    int64  TotalBytes   = 0;

    /** Resolved once in Finish() based on the request's output format. */
    FString OutputFilePath;

    UPROPERTY()
    TObjectPtr<UInoElevenLabsTextToDialogueStream> Action = nullptr;

    /** Remembered at dispatch time so Finish() can pick the right file
     *  extension (.mp3 / .pcm / .ulaw) without reparsing the request. */
    EInoElevenLabsOutputFormat OutputFormat = EInoElevenLabsOutputFormat::Mp3_44100_128;

    UFUNCTION()
    void HandleAudioChunk(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);

    UFUNCTION()
    void HandleComplete(const TArray<uint8>& FullAudioBytes, EInoElevenLabsOutputFormat Format);

    UFUNCTION()
    void HandleError(FString ErrorMessage);

private:
    void Finish();
};
