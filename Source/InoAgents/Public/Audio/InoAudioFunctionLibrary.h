// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "InoAudioFunctionLibrary.generated.h"

/**
 * Audio helper functions exposed to Blueprint.
 *
 * Currently scoped to small utilities that pair well with the
 * RuntimeAudioImporter UStreamingSoundWave's AppendAudioDataFromRAW
 * path (Int16 PCM bytes).
 */
UCLASS()
class INOAGENTS_API UInoAudioFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Generate a zero-filled (silent) raw PCM byte buffer.
     *
     * Output is Int16 little-endian, interleaved across channels —
     * the format expected by AppendAudioDataFromRAW with
     * ERuntimeRAWAudioFormat::Int16. Useful for inserting silence
     * gaps into a streaming wave (between sentences, padding for
     * sync, etc.).
     *
     * @param DurationMs    Length of silence in milliseconds. Clamped
     *                      to >= 0; 0 returns an empty array.
     * @param SampleRate    Sample rate in Hz (typically 16000, 22050,
     *                      24000, 44100). Clamped to [1, 192000].
     * @param NumChannels   Channel count (1 = mono, 2 = stereo).
     *                      Clamped to [1, 8].
     * @return              ByteCount = (DurationMs/1000) * SampleRate
     *                                 * NumChannels * 2 zero bytes.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio",
              meta = (DisplayName = "Generate Empty Raw Audio (Int16)"))
    static TArray<uint8> GenerateEmptyRawAudio(
        float DurationMs,
        int32 SampleRate,
        int32 NumChannels);
};
