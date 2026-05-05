// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLiteRtLmSettings.h"

#include "Misc/Paths.h"

UInoLiteRtLmSettings::UInoLiteRtLmSettings()
{
    // Default model entries — public Hugging Face repos, no auth
    // needed. Most-tested model is Gemma 4; users can add other
    // .litertlm models (Gemma 3, Qwen 2.5, Phi-4-mini, Llama-3.2,
    // etc.) via Project Settings → Plugins → InoLiteRtLm → Models.
    {
        FInoLiteRtLmModelEntry E;
        E.DisplayName  = TEXT("Gemma 4 E2B");
        E.DownloadUrl  = TEXT("https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm");
        E.LocalFileName = TEXT("gemma-4-E2B-it.litertlm");
        E.Language     = TEXT("multi");
        E.Quantization = TEXT("INT4");
        Models.Add(MoveTemp(E));
    }
    {
        FInoLiteRtLmModelEntry E;
        E.DisplayName  = TEXT("Gemma 4 E4B");
        E.DownloadUrl  = TEXT("https://huggingface.co/litert-community/gemma-4-E4B-it-litert-lm/resolve/main/gemma-4-E4B-it.litertlm");
        E.LocalFileName = TEXT("gemma-4-E4B-it.litertlm");
        E.Language     = TEXT("multi");
        E.Quantization = TEXT("INT4");
        Models.Add(MoveTemp(E));
    }
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
        return nullptr;
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
        TEXT("InoAgents"),
        TEXT("Models"));
}

FString UInoLiteRtLmSettings::ResolveLocalPath(const FString& LocalFileName)
{
    if (LocalFileName.IsEmpty())
    {
        return FString();
    }
    return FPaths::Combine(GetModelsDir(), LocalFileName);
}
