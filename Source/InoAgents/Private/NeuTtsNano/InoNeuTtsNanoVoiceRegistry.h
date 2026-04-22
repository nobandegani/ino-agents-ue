// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * One loaded voice entry — reference-text transcript + the pre-encoded
 * FSQ speech-token sequence produced by NeuCodec's PyTorch encoder
 * (see Plugins/InoAgents/NeuTtsNano/scripts/encode-default-voice.py).
 *
 * Empty RefCodes indicates a placeholder voice — the committed
 * default_voice.nvoice.json ships empty until it's regenerated from
 * a real WAV. The registry loads placeholders as-is; the synthesis
 * worker (Milestone 4) null-checks IsPlaceholder() and fails with a
 * clear "regenerate via encode-default-voice.py" error.
 */
struct FInoNeuTtsNanoVoice
{
    FString       DisplayName;
    FString       RefText;
    TArray<int32> RefCodes;

    bool IsPlaceholder() const { return RefCodes.Num() == 0; }
};

/**
 * In-memory map of registered voices. Populated at subsystem
 * Initialize time from Plugins/InoAgents/NeuTtsNano/Resources/
 * default_voice.nvoice.json; future milestones may extend to scan
 * a user-provided voices/ directory.
 *
 * Lookup is by FName (matches the Blueprint-facing voice name like
 * "Default"). The registry is non-copyable, move-only — owned as a
 * TUniquePtr member of UInoNeuTtsNanoSubsystem.
 */
class FInoNeuTtsNanoVoiceRegistry
{
public:
    FInoNeuTtsNanoVoiceRegistry() = default;
    ~FInoNeuTtsNanoVoiceRegistry() = default;

    FInoNeuTtsNanoVoiceRegistry(const FInoNeuTtsNanoVoiceRegistry&) = delete;
    FInoNeuTtsNanoVoiceRegistry& operator=(const FInoNeuTtsNanoVoiceRegistry&) = delete;
    FInoNeuTtsNanoVoiceRegistry(FInoNeuTtsNanoVoiceRegistry&&) = default;
    FInoNeuTtsNanoVoiceRegistry& operator=(FInoNeuTtsNanoVoiceRegistry&&) = default;

    /**
     * Load a .nvoice.json file and register the resulting voice under
     * the given FName. Returns true on happy path, false + OutError
     * on any failure (file not found, JSON parse, missing fields).
     *
     * JSON schema (matches what encode-default-voice.py emits):
     *   {
     *     "display_name": "Default",
     *     "ref_text": "<verbatim transcript>",
     *     "ref_codes": [int, int, ...]
     *   }
     */
    bool RegisterFromJsonFile(const FString& JsonPath, FName RegisterAs, FString& OutError);

    /** Register a voice in-place. Replaces any existing entry at Name. */
    void Register(FName Name, FInoNeuTtsNanoVoice Voice);

    /** Lookup. Returns nullptr if Name isn't registered. */
    const FInoNeuTtsNanoVoice* Find(FName Name) const;

    /** Used by UInoNeuTtsNanoSubsystem::GetAvailableVoiceNames to
     *  surface the voice list to Blueprint. */
    TArray<FName> GetAvailableVoiceNames() const;

    int32 Num() const { return Voices.Num(); }

private:
    TMap<FName, FInoNeuTtsNanoVoice> Voices;
};
