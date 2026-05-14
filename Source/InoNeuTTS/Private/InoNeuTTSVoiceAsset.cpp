// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSVoiceAsset.h"

#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

FInoNeuTTSVoice UInoNeuTTSVoiceAsset::ToRuntimeVoice() const
{
    FInoNeuTTSVoice V;
    V.Name      = Name;
    V.Language  = Language;
    V.RefText   = RefText;
    V.RefPhones = RefPhones;
    V.RefCodes  = RefCodes;
    V.bIsValid  = IsUsable();
    return V;
}

void UInoNeuTTSVoiceAsset::PostInitProperties()
{
    Super::PostInitProperties();
#if WITH_EDITORONLY_DATA
    // Construct the import-data sub-object so UInoNeuTTSVoiceFactory has
    // somewhere to record the source `.inv` path. Skip on the CDO — it
    // doesn't need its own import data and creating sub-objects there
    // would leak across instances.
    if (!HasAnyFlags(RF_ClassDefaultObject))
    {
        AssetImportData = NewObject<UAssetImportData>(
            this, TEXT("AssetImportData"));
    }
#endif
}

#if WITH_EDITOR
void UInoNeuTTSVoiceAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName ChangedProperty = PropertyChangedEvent.GetPropertyName();
    if (ChangedProperty == GET_MEMBER_NAME_CHECKED(UInoNeuTTSVoiceAsset, RefText))
    {
        // RefText changed → invalidate the cached phonemization so the
        // runner re-phonemizes via InoSpeakNG on the next synth.
        // RefCodes are intentionally NOT cleared — they come from the
        // offline encoder and survive a transcript fix.
        RefPhones.Reset();
    }
}
#endif

#if WITH_EDITORONLY_DATA
void UInoNeuTTSVoiceAsset::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Super::GetAssetRegistryTags(Context);
    Context.AddTag(FAssetRegistryTag(
        TEXT("VoiceName"), Name, FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(FAssetRegistryTag(
        TEXT("Language"), Language, FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(FAssetRegistryTag(
        TEXT("RefCodeCount"), FString::FromInt(RefCodes.Num()),
        FAssetRegistryTag::TT_Numerical));
    Context.AddTag(FAssetRegistryTag(
        TEXT("HasRefPhones"), RefPhones.IsEmpty() ? TEXT("false") : TEXT("true"),
        FAssetRegistryTag::TT_Alphabetical));
}
#endif
