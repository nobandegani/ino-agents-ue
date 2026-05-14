// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSVoiceAsset.h"

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
