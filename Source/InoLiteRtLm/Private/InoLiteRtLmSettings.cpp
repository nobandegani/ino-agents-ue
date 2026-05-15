// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLiteRtLmSettings.h"

#include "Misc/Paths.h"

UInoLiteRtLmSettings::UInoLiteRtLmSettings()
{
    // No defaults — arrays start empty. Configure entries in
    // Project Settings → Plugins → InoLiteRtLm → Models.
    // Same convention as UInoNeuTtsNativeSettings: the runtime is
    // model-agnostic, so the registry should not privilege any
    // particular .litertlm bundle.
}

const FInoLiteRtLmModelEntry* UInoLiteRtLmSettings::FindModelByFileName(
    const FString& FileName) const
{
    for (const FInoLiteRtLmModelEntry& Entry : Models)
    {
        if (Entry.LocalFileName.Equals(FileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

const FInoLiteRtLmModelEntry* UInoLiteRtLmSettings::FindModel(
    const FString& NameOrFileName) const
{
    if (NameOrFileName.IsEmpty())
    {
        // Empty → pick first registry entry, matching InoNeuTTS's
        // FindBackbone/FindDecoder forgiving lookup. Lets callers leave
        // FInoLiteRtLmModelConfig::ModelFileName at its default empty
        // value and "just get whatever's configured first" rather than
        // having to know the exact LocalFileName up front.
        return Models.Num() > 0 ? &Models[0] : nullptr;
    }

    // Prefer exact file-name match — if someone has a model named
    // "Gemma 4 E2B" as a display name but ALSO a different entry with
    // that literal file name, the file-name path should win (it's more
    // specific / authoritative).
    if (const FInoLiteRtLmModelEntry* Entry = FindModelByFileName(NameOrFileName))
    {
        return Entry;
    }

    // Fall back to display-name match.
    for (const FInoLiteRtLmModelEntry& Entry : Models)
    {
        if (Entry.DisplayName.Equals(NameOrFileName, ESearchCase::IgnoreCase))
        {
            return &Entry;
        }
    }
    return nullptr;
}

FString UInoLiteRtLmSettings::GetModelsDir()
{
    return FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("ino-agents"),
        TEXT("lite-rt-lm"));
}

FString UInoLiteRtLmSettings::ResolveLocalPath(const FString& LocalFileName)
{
    if (LocalFileName.IsEmpty())
    {
        return FString();
    }
    return FPaths::Combine(GetModelsDir(), LocalFileName);
}
