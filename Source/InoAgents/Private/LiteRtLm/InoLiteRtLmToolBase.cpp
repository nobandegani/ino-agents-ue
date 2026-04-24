// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmToolBase.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString UInoLiteRtLmToolBase::Execute_Implementation(const FJsonObjectWrapper& Arguments)
{
    return FString();
}

FString UInoLiteRtLmToolBase::ExecuteFromString(const FString& ArgumentsJson)
{
    // Parse the raw JSON string into FJsonObjectWrapper so Execute
    // receives a pre-parsed object. Blueprint tools use GetField
    // nodes on it; C++ tools access Arguments.JsonObject directly.
    FJsonObjectWrapper Wrapper;

    if (!ArgumentsJson.IsEmpty())
    {
        TSharedPtr<FJsonObject> Parsed;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
        if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
        {
            Wrapper.JsonObject = Parsed;
            Wrapper.JsonString = ArgumentsJson;
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Tool: ExecuteFromString failed to parse arguments "
                        "JSON for tool '%s': %s"),
                   *ToolName.ToString(), *ArgumentsJson);
        }
    }

    return Execute(Wrapper);
}

FString UInoLiteRtLmToolBase::BuildSchemaJson() const
{
    // Build OpenAI-style function-call schema from properties:
    //
    //   {
    //     "type": "function",
    //     "function": {
    //       "name": "<ToolName>",
    //       "description": "<Description>",
    //       "parameters": {
    //         "type": "object",
    //         "properties": {
    //           "<param.Name>": {
    //             "type": "<param.Type>",
    //             "description": "<param.Description>"
    //           }, ...
    //         },
    //         "required": ["<param.Name where bRequired>", ...]
    //       }
    //     }
    //   }

    TSharedRef<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> RequiredArray;

    for (const FInoLiteRtLmToolParameter& Param : Parameters)
    {
        TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("type"), Param.Type);
        if (!Param.Description.IsEmpty())
        {
            ParamObj->SetStringField(TEXT("description"), Param.Description);
        }
        PropertiesObj->SetObjectField(Param.Name.ToString(), ParamObj);

        if (Param.bRequired)
        {
            RequiredArray.Add(MakeShared<FJsonValueString>(Param.Name.ToString()));
        }
    }

    TSharedRef<FJsonObject> ParametersObj = MakeShared<FJsonObject>();
    ParametersObj->SetStringField(TEXT("type"), TEXT("object"));
    ParametersObj->SetObjectField(TEXT("properties"), PropertiesObj);
    if (RequiredArray.Num() > 0)
    {
        ParametersObj->SetArrayField(TEXT("required"), RequiredArray);
    }

    TSharedRef<FJsonObject> FunctionObj = MakeShared<FJsonObject>();
    FunctionObj->SetStringField(TEXT("name"), ToolName.ToString());
    FunctionObj->SetStringField(TEXT("description"), Description);
    FunctionObj->SetObjectField(TEXT("parameters"), ParametersObj);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("type"), TEXT("function"));
    Root->SetObjectField(TEXT("function"), FunctionObj);

    FString Result;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
    FJsonSerializer::Serialize(Root, Writer);
    return Result;
}
