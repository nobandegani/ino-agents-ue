// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * One loaded voice entry — reference-text transcript (original +
 * phonemized) + the pre-encoded FSQ speech-token sequence produced
 * by NeuCodec's PyTorch encoder (see
 * Plugins/InoAgents/NeuTtsNanoNative/scripts/encode-default-voice.py).
 *
 * Schema (matches what encode-default-voice.py emits):
 *   - DisplayName: human-readable name for UI / logs
 *   - RefText:     raw transcript of the reference WAV (English, etc.)
 *                  Preserved for diagnostic purposes only — the runtime
 *                  prompt uses RefPhones, not RefText, because NeuTTS
 *                  Nano was trained on IPA phonemes, not raw text.
 *   - RefPhones:   IPA phonemization of RefText via espeak-ng
 *                  (produced offline by the encoder script). This is
 *                  what the Milestone 4 prompt builder concatenates
 *                  with the caller's pre-phonemized target text.
 *   - RefCodes:    FSQ speech-token ids from NeuCodec's encoder.
 *
 * Empty RefCodes indicates a placeholder voice — the committed
 * default_voice.nvoice.json ships empty until it's regenerated from
 * a real WAV. The registry loads placeholders as-is; the synthesis
 * worker (Milestone 4) null-checks IsPlaceholder() and fails with a
 * clear "regenerate via encode-default-voice.py" error.
 */
struct FInoNeuTtsNanoNativeVoice
{
    FString       DisplayName;
    FString       RefText;
    FString       RefPhones;
    TArray<int32> RefCodes;

    bool IsPlaceholder() const { return RefCodes.Num() == 0; }
};

/**
 * In-memory map of registered voices. Populated at subsystem
 * Initialize time from Plugins/InoAgents/NeuTtsNanoNative/Resources/
 * default_voice.nvoice.json; future milestones may extend to scan
 * a user-provided voices/ directory.
 *
 * Lookup is by FName (matches the Blueprint-facing voice name like
 * "Default"). The registry is non-copyable, move-only — owned as a
 * TUniquePtr member of UInoNeuTtsNanoNativeSubsystem.
 */
class FInoNeuTtsNanoNativeVoiceRegistry
{
public:
    FInoNeuTtsNanoNativeVoiceRegistry() = default;
    ~FInoNeuTtsNanoNativeVoiceRegistry() = default;

    FInoNeuTtsNanoNativeVoiceRegistry(const FInoNeuTtsNanoNativeVoiceRegistry&) = delete;
    FInoNeuTtsNanoNativeVoiceRegistry& operator=(const FInoNeuTtsNanoNativeVoiceRegistry&) = delete;
    FInoNeuTtsNanoNativeVoiceRegistry(FInoNeuTtsNanoNativeVoiceRegistry&&) = default;
    FInoNeuTtsNanoNativeVoiceRegistry& operator=(FInoNeuTtsNanoNativeVoiceRegistry&&) = default;

    /**
     * Load a .nvoice.json file and register the resulting voice under
     * the given FName. Returns true on happy path, false + OutError
     * on any failure (file not found, JSON parse, missing fields).
     *
     * JSON schema (matches what encode-default-voice.py emits):
     *   {
     *     "display_name": "Default",
     *     "ref_text":     "<verbatim transcript in source language>",
     *     "ref_phones":   "<IPA phonemization via espeak-ng>",
     *     "ref_codes":    [int, int, ...]
     *   }
     *
     * ref_phones is the field the runtime prompt builder actually
     * consumes. ref_text is kept for diagnostic / debugging purposes
     * (showing the original transcript in logs).
     */
    bool RegisterFromJsonFile(const FString& JsonPath, FName RegisterAs, FString& OutError);

    /** Register a voice in-place. Replaces any existing entry at Name. */
    void Register(FName Name, FInoNeuTtsNanoNativeVoice Voice);

    /** Lookup. Returns nullptr if Name isn't registered. */
    const FInoNeuTtsNanoNativeVoice* Find(FName Name) const;

    /** Used by UInoNeuTtsNanoNativeSubsystem::GetAvailableVoiceNames to
     *  surface the voice list to Blueprint. */
    TArray<FName> GetAvailableVoiceNames() const;

    int32 Num() const { return Voices.Num(); }

private:
    TMap<FName, FInoNeuTtsNanoNativeVoice> Voices;
};
