// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ElevenLabs/ElevenLabsSettings.h"

namespace
{
    constexpr const TCHAR* kDefaultBaseUrl = TEXT("https://api.elevenlabs.io");
}

FString UElevenLabsSettings::GetEffectiveBaseUrl() const
{
    FString Result = BaseUrl.IsEmpty() ? FString(kDefaultBaseUrl) : BaseUrl;

    // Strip any trailing slashes so callers can safely append "/v1/..."
    // without producing "api.elevenlabs.io//v1/...", which ElevenLabs'
    // edge routes 404. Users who paste the URL from the docs sometimes
    // include the trailing slash.
    while (Result.EndsWith(TEXT("/")))
    {
        Result.LeftChopInline(1);
    }

    return Result;
}
