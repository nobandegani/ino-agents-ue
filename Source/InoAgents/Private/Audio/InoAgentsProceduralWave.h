// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Templates/Atomic.h"

#include "InoAgentsProceduralWave.generated.h"

class UInoAgentsStreamingAudioComponent;

/**
 * Custom USoundWaveProcedural subclass that intercepts GeneratePCMData
 * on the audio render thread to capture the exact samples being played.
 *
 * The captured int16 samples are converted to normalized floats and
 * accumulated. When the accumulator reaches the batch size, the data
 * is dispatched to the game thread via AsyncTask and broadcast through
 * the owning audio component's OnGeneratePCMData delegate.
 *
 * Threading: GeneratePCMData runs on the audio render thread.
 * SetOwnerAndBatchSize / ResetVisualization run on the game thread.
 * An atomic flag (bResetPending) coordinates between them so the
 * Accumulator is never modified from both threads simultaneously.
 */
UCLASS()
class UInoAgentsProceduralWave : public USoundWaveProcedural
{
    GENERATED_BODY()

public:
    /** Set the owning component and batch size. Call after construction.
     *  Game thread only. */
    void SetOwnerAndBatchSize(UInoAgentsStreamingAudioComponent* InOwner,
                              int32 InBatchSize);

    /** Signal that the accumulator should be cleared. The actual clear
     *  happens on the audio thread at the start of the next
     *  GeneratePCMData call. Game thread only. */
    void ResetVisualization();

    //~ USoundWaveProcedural interface
    virtual int32 GeneratePCMData(uint8* PCMData, const int32 SamplesNeeded) override;
    //~ End USoundWaveProcedural interface

private:
    TWeakObjectPtr<UInoAgentsStreamingAudioComponent> OwnerWeak;
    TAtomic<int32> BatchSize{0};

    /** Set by ResetVisualization (game thread), consumed by
     *  GeneratePCMData (audio thread). Ensures the Accumulator is
     *  only cleared on the audio thread where it's written. */
    TAtomic<bool> bResetPending{false};

    /** Accumulator. ONLY accessed from the audio render thread
     *  (inside GeneratePCMData). Game thread signals resets via
     *  bResetPending, never touches Accumulator directly. */
    TArray<float> Accumulator;
};
