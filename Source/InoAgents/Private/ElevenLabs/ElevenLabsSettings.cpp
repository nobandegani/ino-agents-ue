// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ElevenLabs/ElevenLabsSettings.h"

namespace
{
    constexpr const TCHAR* kDefaultBaseUrl = TEXT("https://api.elevenlabs.io");
}

FString UElevenLabsSettings::GetEffectiveBaseUrl() const
{
    return BaseUrl.IsEmpty() ? FString(kDefaultBaseUrl) : BaseUrl;
}
