// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoLiteRtLmSettings.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

const char* LiteRtLmBackendToString(EInoLiteRtLmBackend Backend)
{
    switch (Backend)
    {
        case EInoLiteRtLmBackend::Cpu: return "cpu";
        case EInoLiteRtLmBackend::Gpu: return "gpu";
        case EInoLiteRtLmBackend::Npu: return "npu";
    }
    return "cpu";
}

FString LiteRtLmResolveModelPath(const FString& LocalFileName)
{
    if (LocalFileName.IsEmpty())
    {
        return FString();
    }

    // 1. PersistentDownloadDir — where the InoNodes downloader writes
    //    cached models. Same path UInoLiteRtLmSettings::ResolveLocalPath
    //    builds, so resolve / download / load all agree on one location.
    {
        const FString Path = UInoLiteRtLmSettings::ResolveLocalPath(LocalFileName);
        if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
        {
            return FPaths::ConvertRelativePathToFull(Path);
        }
    }

    // 2. Plugin directory — legacy dev path (manual drop into
    //    Plugins/InoAgents/LiteRTLM/). Kept for backward compat with
    //    pre-PersistentDownloadDir workflows.
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (Plugin.IsValid())
        {
            const FString Path = FPaths::Combine(
                Plugin->GetBaseDir(), TEXT("LiteRTLM"), LocalFileName);
            if (IFileManager::Get().FileExists(*Path))
            {
                return FPaths::ConvertRelativePathToFull(Path);
            }
        }
    }

    // 3. Not found.
    return FString();
}
