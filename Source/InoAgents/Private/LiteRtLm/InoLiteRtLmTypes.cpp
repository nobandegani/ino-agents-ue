// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

const char* LiteRtLmBackendToString(EInoLiteRtLmBackend Backend)
{
    switch (Backend)
    {
        case EInoLiteRtLmBackend::Cpu: return "cpu";
        case EInoLiteRtLmBackend::Gpu: return "gpu";
    }
    return "cpu";
}

FString LiteRtLmResolveModelPath(const FString& ModelFileName)
{
    if (ModelFileName.IsEmpty())
    {
        return FString();
    }

    // 1. PersistentDownloadDir — downloaded / cached models (dev + shipping).
    {
        const FString Path = FPaths::Combine(
            FPaths::ProjectPersistentDownloadDir(),
            TEXT("InoAgents"), TEXT("Models"), ModelFileName);
        if (IFileManager::Get().FileExists(*Path))
        {
            return FPaths::ConvertRelativePathToFull(Path);
        }
    }

    // 2. Plugin directory — legacy dev path (manual drop into Plugins/InoAgents/Models/).
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (Plugin.IsValid())
        {
            const FString Path = FPaths::Combine(
                Plugin->GetBaseDir(), TEXT("Models"), ModelFileName);
            if (IFileManager::Get().FileExists(*Path))
            {
                return FPaths::ConvertRelativePathToFull(Path);
            }
        }
    }

    // 3. Not found.
    return FString();
}
