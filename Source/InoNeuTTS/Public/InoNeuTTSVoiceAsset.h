// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // FInoNeuTTSVoice

#include "InoNeuTTSVoiceAsset.generated.h"

/**
 * Blueprint-friendly UAsset wrapper around an encoded NeuTTS voice.
 *
 * One asset = one voice. Holds the reference transcript, optional
 * pre-baked IPA phonemization, and the NeuCodec FSQ codes that
 * represent the voice. Constructed either:
 *
 *   (a) programmatically via `NewObject<UInoNeuTTSVoiceAsset>` +
 *       populating the fields directly (C++ / Blueprint), OR
 *   (b) at runtime by a JSON loader the caller provides (we don't
 *       ship one yet — the editor UFactory + .inv import workflow
 *       was deferred per the original Phase 2 scope).
 *
 * Passed to UInoNeuTTSSubsystem::SetActiveVoiceAsync to prime the
 * runner for synthesis. The subsystem calls ToRuntimeVoice() to
 * convert this asset to the runner-side FInoNeuTTSVoice struct.
 *
 * Edit-time hook: changing RefText clears RefPhones so the runner
 * re-phonemizes via InoSpeakNG on next synth.
 */
UCLASS(BlueprintType, hidecategories=(Object))
class INONEUTTS_API UInoNeuTTSVoiceAsset : public UObject
{
    GENERATED_BODY()

public:
    /** Voice identifier / cache key used by FInoNeuTTSRunner. Should be
     *  short, alpha-only, and unique within a project (e.g. "jo",
     *  "dave", "narrator"). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice")
    FString Name;

    /** eSpeak language code matching the reference audio's language.
     *  Common values: "en-us", "en-gb", "de", "fr-fr", "es", "ja",
     *  "multi". Drives input-text phonemization at synth time. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice")
    FString Language = TEXT("en-us");

    /** Transcript of the reference WAV the voice was encoded from.
     *  Used at synth time to provide phoneme alignment context.
     *  Editing this clears RefPhones (see PostEditChangeProperty). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice",
              meta = (MultiLine = true))
    FString RefText;

    /** Optional pre-baked IPA phonemization of RefText. Set by the
     *  offline encoder. Empty → InoSpeakNG re-phonemizes at synth time
     *  (slightly slower per-call, but no offline tooling required). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice|Advanced")
    FString RefPhones;

    /** NeuCodec FSQ codes (50 Hz, single codebook). Produced by the
     *  offline encoder from the reference WAV. ~650 codes for a 13-second
     *  reference clip. Manual edits break the voice — re-encode the
     *  source WAV to refresh. Each value is in [0, 65535]. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice|Advanced")
    TArray<int32> RefCodes;

    /** True if Name + Language + RefCodes are all set. UMG-safe
     *  property to gate "Set Active Voice" buttons. */
    UFUNCTION(BlueprintPure, Category = "Voice")
    bool IsUsable() const
    {
        return RefCodes.Num() > 0 && !Name.IsEmpty() && !Language.IsEmpty();
    }

    /** Convert this asset's data to the runner-side FInoNeuTTSVoice
     *  struct. Called by UInoNeuTTSSubsystem internally; not normally
     *  invoked from Blueprint (the subsystem owns the asset → runtime
     *  conversion). */
    FInoNeuTTSVoice ToRuntimeVoice() const;

#if WITH_EDITOR
    /** Clear RefPhones whenever RefText is edited so the runner
     *  re-phonemizes on next synth. RefCodes are NOT cleared — those
     *  come from the offline encoder; editing RefText without
     *  re-encoding produces a stale-but-usable voice rather than a
     *  broken one. */
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
