// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsVoiceAsset.h"
#include "InoNeuTtsLog.h"

#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/AssetRegistryTagsContext.h"
#endif

FInoNeuTtsVoice UInoNeuTtsVoiceAsset::ToRuntimeVoice() const
{
	FInoNeuTtsVoice V;
	V.Name      = Name;
	V.Language  = Language;
	V.RefText   = RefText;
	V.RefPhones = RefPhones;
	V.RefCodes  = RefCodes;
	V.bIsValid  = IsUsable();
	return V;
}

void UInoNeuTtsVoiceAsset::PostInitProperties()
{
	Super::PostInitProperties();

#if WITH_EDITORONLY_DATA
	// Lazy-create the import-data UObject the first time the asset
	// is touched. Any factory that imports this asset will populate
	// it via AssetImportData->Update(SourceFilePath) — see
	// UInoNeuTtsVoiceFactory in the editor module.
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		if (AssetImportData == nullptr)
		{
			AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
		}
	}
#endif
}

#if WITH_EDITOR
void UInoNeuTtsVoiceAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName ChangedName = PropertyChangedEvent.GetPropertyName();

	// If the user edited RefText we need to invalidate the cached
	// RefPhones so the runtime re-phonemizes on next synth via
	// InoSpeakNG. RefPhones itself is VisibleAnywhere (read-only) so
	// it can't be the changed property here.
	if (ChangedName == GET_MEMBER_NAME_CHECKED(UInoNeuTtsVoiceAsset, RefText))
	{
		if (!RefPhones.IsEmpty())
		{
			UE_LOG(LogInoNeuTts, Log,
				TEXT("VoiceAsset '%s': RefText edited — clearing pre-baked RefPhones; ")
				TEXT("runtime will re-phonemize via InoSpeakNG."),
				*Name);
			RefPhones.Reset();
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UInoNeuTtsVoiceAsset::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	// These tags drive Content Browser column display + filtering. Adding
	// the most useful at-a-glance fields lets you scan a folder of voices
	// without opening each asset.
	Context.AddTag(FAssetRegistryTag(TEXT("VoiceName"),   Name,
		FAssetRegistryTag::TT_Alphabetical));
	Context.AddTag(FAssetRegistryTag(TEXT("Language"),    Language,
		FAssetRegistryTag::TT_Alphabetical));
	Context.AddTag(FAssetRegistryTag(TEXT("RefCodeCount"), FString::FromInt(RefCodes.Num()),
		FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag(TEXT("HasRefPhones"), RefPhones.IsEmpty() ? TEXT("false") : TEXT("true"),
		FAssetRegistryTag::TT_Alphabetical));

	Super::GetAssetRegistryTags(Context);
}
#endif
