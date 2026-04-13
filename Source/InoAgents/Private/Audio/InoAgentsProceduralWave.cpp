// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgentsProceduralWave.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"

#include "Async/Async.h"

void UInoAgentsProceduralWave::SetOwnerAndBatchSize(
    UInoAgentsStreamingAudioComponent* InOwner, int32 InBatchSize)
{
    OwnerWeak = InOwner;
    BatchSize = FMath::Max(InBatchSize, 0);
    // Signal a reset — the audio thread will clear the accumulator
    // at the start of the next GeneratePCMData call.
    bResetPending = true;
}

void UInoAgentsProceduralWave::ResetVisualization()
{
    OwnerWeak.Reset();
    BatchSize = 0;
    // Signal a reset instead of touching Accumulator directly —
    // Accumulator is only safe to modify on the audio thread.
    bResetPending = true;
}

int32 UInoAgentsProceduralWave::GeneratePCMData(uint8* PCMData, const int32 SamplesNeeded)
{
    // Consume the reset flag (set by game thread). This is the ONLY
    // place Accumulator is modified — always on the audio thread.
    if (bResetPending.Exchange(false))
    {
        Accumulator.Reset();
        const int32 CurrentBatch = BatchSize.Load();
        if (CurrentBatch > 0)
        {
            Accumulator.Reserve(CurrentBatch);
        }
    }

    // Let the base class pull samples from the queue into PCMData.
    const int32 BytesGenerated = Super::GeneratePCMData(PCMData, SamplesNeeded);

    // Skip visualization if disabled, nobody is listening, or the
    // queue is empty (underrun — GeneratePCMData returns zeros when
    // there's no real audio data, and we don't want to broadcast
    // silence as visualization data).
    const int32 CurrentBatchSize = BatchSize.Load();
    if (CurrentBatchSize <= 0 || !OwnerWeak.IsValid() || BytesGenerated <= 0
        || GetAvailableAudioByteCount() <= 0)
    {
        return BytesGenerated;
    }

    // Convert the played int16 samples to normalized floats and
    // accumulate. When the accumulator fills, dispatch to game thread.
    const int32 NumSamples = BytesGenerated / sizeof(int16);
    const int16* Samples = reinterpret_cast<const int16*>(PCMData);

    for (int32 i = 0; i < NumSamples; ++i)
    {
        Accumulator.Add(static_cast<float>(Samples[i]) / 32768.0f);

        if (Accumulator.Num() >= CurrentBatchSize)
        {
            // Move the batch off the audio thread — capture by value.
            TArray<float> Batch = MoveTemp(Accumulator);
            Accumulator.Reset();
            Accumulator.Reserve(CurrentBatchSize);

            TWeakObjectPtr<UInoAgentsStreamingAudioComponent> WeakOwner = OwnerWeak;
            AsyncTask(ENamedThreads::GameThread,
                [WeakOwner, Batch = MoveTemp(Batch)]()
            {
                if (UInoAgentsStreamingAudioComponent* Comp = WeakOwner.Get())
                {
                    Comp->OnGeneratePCMData.Broadcast(Batch);
                }
            });
        }
    }

    return BytesGenerated;
}
