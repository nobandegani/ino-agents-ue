// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoSmokeTestCommon.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace InoSmokeTest
{

FString ResolveDefaultModelPath()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SmokeTest: IPluginManager could not locate the InoAgents plugin. "
                    "This should be impossible — something is very wrong with the module state."));
        return FString();
    }

    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(
        BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SmokeTest: model file not found at %s. Download from "
                    "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm "
                    "and save as Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm"),
               *ModelPath);
        return FString();
    }

    return ModelPath;
}

TSharedPtr<FJsonObject> ParseJsonObjectOrLog(const FString& Json, const TCHAR* Context)
{
    TSharedPtr<FJsonObject> Obj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("%s: failed to parse JSON: %s"),
               Context, *Json);
        return nullptr;
    }
    return Obj;
}

bool TryExtractFirstToolCall(const TSharedPtr<FJsonObject>& ResponseObj,
                             FString& OutToolName,
                             TSharedPtr<FJsonObject>& OutArgumentsObj)
{
    const TArray<TSharedPtr<FJsonValue>>* ToolCallsArrayPtr = nullptr;
    if (!ResponseObj->TryGetArrayField(TEXT("tool_calls"), ToolCallsArrayPtr)
        || ToolCallsArrayPtr == nullptr
        || ToolCallsArrayPtr->Num() == 0)
    {
        OutArgumentsObj = MakeShared<FJsonObject>();
        return false;
    }

    const TSharedPtr<FJsonValue>& FirstToolCallValue = (*ToolCallsArrayPtr)[0];
    if (!FirstToolCallValue.IsValid()
        || FirstToolCallValue->Type != EJson::Object)
    {
        OutArgumentsObj = MakeShared<FJsonObject>();
        return false;
    }
    const TSharedPtr<FJsonObject>& FirstToolCallObj = FirstToolCallValue->AsObject();

    const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
    if (!FirstToolCallObj->TryGetObjectField(TEXT("function"), FunctionObjPtr)
        || FunctionObjPtr == nullptr)
    {
        OutArgumentsObj = MakeShared<FJsonObject>();
        return false;
    }
    const TSharedPtr<FJsonObject>& FunctionObj = *FunctionObjPtr;

    if (!FunctionObj->TryGetStringField(TEXT("name"), OutToolName))
    {
        OutArgumentsObj = MakeShared<FJsonObject>();
        return false;
    }

    // arguments may be either a JSON object or absent (for zero-arg tools).
    const TSharedPtr<FJsonObject>* ArgumentsObjPtr = nullptr;
    if (FunctionObj->TryGetObjectField(TEXT("arguments"), ArgumentsObjPtr)
        && ArgumentsObjPtr != nullptr)
    {
        OutArgumentsObj = *ArgumentsObjPtr;
    }
    else
    {
        OutArgumentsObj = MakeShared<FJsonObject>();
    }

    return true;
}

FString ExtractAssistantText(const TSharedPtr<FJsonObject>& ResponseObj)
{
    const TArray<TSharedPtr<FJsonValue>>* ContentArrayPtr = nullptr;
    if (!ResponseObj->TryGetArrayField(TEXT("content"), ContentArrayPtr)
        || ContentArrayPtr == nullptr)
    {
        return FString();
    }

    FString Result;
    for (const TSharedPtr<FJsonValue>& PartValue : *ContentArrayPtr)
    {
        if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
        {
            continue;
        }
        const TSharedPtr<FJsonObject>& PartObj = PartValue->AsObject();

        FString PartType;
        PartObj->TryGetStringField(TEXT("type"), PartType);
        if (PartType != TEXT("text"))
        {
            continue;
        }

        FString PartText;
        if (PartObj->TryGetStringField(TEXT("text"), PartText))
        {
            if (!Result.IsEmpty())
            {
                Result += TEXT(" ");
            }
            Result += PartText;
        }
    }
    return Result;
}

}  // namespace InoSmokeTest
