// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"
#include "UObject/Object.h"

#include "InoNeuTtsVoiceAsset.generated.h"

#if WITH_EDITORONLY_DATA
class UAssetImportData;
#endif

/**
 * Editor / runtime UAsset wrapper around a NeuTTS reference voice.
 *
 * On disk the source format is `.inv` — a plain JSON file with the same
 * fields the legacy `.nvoice.json` voice registry parses. Drag/drop a
 * `.inv` into the Content Browser and the InoNeuTtsNativeEditor module's
 * import factory turns it into one of these UAsset instances.
 *
 * Once imported, the asset can be:
 *   - Soft-referenced from Blueprint (TSoftObjectPtr<UInoNeuTtsVoiceAsset>),
 *     so a level / character actor can pick "the voice for this NPC" the
 *     same way it picks any other UAsset.
 *   - Passed to UInoNeuTtsSubsystem::SetActiveVoiceFromAssetAsync, which
 *     converts to the lower-level FInoNeuTtsVoice struct via ToRuntimeVoice
 *     and routes through the existing prime / synth pipeline.
 *
 * Field policy:
 *   - Name / Language / RefText are EDITABLE — they're metadata and the
 *     user can tweak them.
 *   - RefPhones is regenerated lazily at runtime if RefText changes
 *     (PostEditChangeProperty clears it). The build-voices.py offline
 *     script bakes RefPhones once for hot-path performance, but the
 *     runtime path can recover via InoSpeakNG.
 *   - RefCodes are READ-ONLY in the editor. They're the actual NeuCodec
 *     FSQ tokens (~650 ints for a 13 s reference); manual editing breaks
 *     the voice. Re-import the source `.inv` to refresh them.
 */
UCLASS(BlueprintType, hidecategories = (Object))
class INONEUTTSNATIVE_API UInoNeuTtsVoiceAsset : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Voice identifier used as the cache key on UInoNeuTtsSubsystem
	 * (e.g. "jo", "dave"). Must be unique across voices the project
	 * primes simultaneously — re-using a name would collide with the
	 * existing primed voice's cache entry.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice")
	FString Name;

	/**
	 * eSpeak language code for input + reference phonemization
	 * (e.g. "en-us", "de", "fr-fr", "es"). Must match what the chosen
	 * NeuTTS backbone was trained on — see the BACKBONE_LANGUAGE_MAP
	 * in vendor/neutts/neutts.py for the per-backbone languages.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice")
	FString Language = TEXT("en-us");

	/**
	 * Plain-text transcript of the source reference WAV. Used as the
	 * "reference text" half of the prompt before InputPhones. If you
	 * edit this in the editor, RefPhones is cleared so the runtime
	 * re-phonemizes via InoSpeakNG on next synth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice", meta = (MultiLine = true))
	FString RefText;

	/**
	 * IPA phonemization of RefText, pre-baked offline by
	 * `Plugins/InoAgents/NeuTTS/scripts/build-voices.py`. Empty here is
	 * legal — the runtime falls back to live phonemization. Read-only
	 * because the source of truth is the offline build (or a runtime
	 * regeneration); manual edits would just be overwritten on the
	 * next prime / re-import.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice|Advanced")
	FString RefPhones;

	/**
	 * NeuCodec FSQ speech tokens (50 Hz code rate, single codebook).
	 * Typically ~650 ints for a 13 s reference. Read-only — these
	 * come from running the source WAV through NeuCodec's encoder
	 * offline. Re-import the source `.inv` to update.
	 *
	 * Hidden behind the "Advanced" subcategory because it's a 650-int
	 * blob, useless for the user to scroll through but still accessible
	 * for diagnostics.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice|Advanced")
	TArray<int32> RefCodes;

#if WITH_EDITORONLY_DATA
	/**
	 * Source file tracking — drives the "Reimport" Content Browser
	 * action. Populated by UInoNeuTtsVoiceFactory after import; UE
	 * keeps it in sync via the FReimportHandler interface in the
	 * editor module.
	 */
	UPROPERTY(VisibleAnywhere, Instanced, Category = "ImportSettings")
	TObjectPtr<UAssetImportData> AssetImportData;
#endif

	// ---- Convenience ----

	/**
	 * True iff the asset is usable for synthesis: has RefCodes, a
	 * non-empty Name, and a non-empty Language. RefText / RefPhones
	 * can be empty (live phonemization handles either one missing).
	 */
	UFUNCTION(BlueprintPure, Category = "Voice")
	bool IsUsable() const
	{
		return RefCodes.Num() > 0
			&& !Name.IsEmpty()
			&& !Language.IsEmpty();
	}

	/**
	 * Build the lower-level runtime struct used by the synth pipeline.
	 * C++-only — FInoNeuTtsVoice is not BlueprintType. Blueprint code
	 * should hand the asset to UInoNeuTtsSubsystem::SetActiveVoiceAsync
	 * directly; that overload calls this internally.
	 *
	 * Cheap (TArray + FString copies); call freely from C++.
	 */
	FInoNeuTtsVoice ToRuntimeVoice() const;

	// ---- UObject ----

#if WITH_EDITOR
	/** Editor hook: clear RefPhones when RefText changes so the next
	 *  synth re-phonemizes via InoSpeakNG. RefPhones isn't editable
	 *  itself, so the only paths that update it are import/re-import
	 *  and runtime synth. */
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Editor hook: surface Name, Language, and RefCodes count in the
	 *  Asset Registry so Content Browser filters can pick on them.
	 *  Uses the post-5.4 context-based API. */
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif

	virtual void PostInitProperties() override;
};
