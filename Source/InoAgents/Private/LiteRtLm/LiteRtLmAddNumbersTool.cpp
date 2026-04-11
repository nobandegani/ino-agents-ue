// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmAddNumbersTool.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FName ULiteRtLmAddNumbersTool::GetToolName_Implementation() const
{
    return TEXT("add_numbers");
}

FString ULiteRtLmAddNumbersTool::GetToolSchemaJson_Implementation() const
{
    // OpenAI-style function schema. The "name" field MUST match
    // GetToolName() or the subsystem's BuildToolsJsonForConversation
    // will refuse to register this tool. The description is the
    // single most important field for getting Gemma 4 E2B to
    // actually USE the tool instead of computing inline — phrasing
    // it as an explicit instruction ("Always use this tool when...")
    // moves the needle significantly. This exact text is the one
    // that worked in the Phase 1 ToolCallTest.
    return FString(TEXT(R"({
        "type": "function",
        "function": {
            "name": "add_numbers",
            "description": "Adds two integers and returns their sum. Always use this tool when the user asks to add, sum, or total two numbers — do not compute in your head.",
            "parameters": {
                "type": "object",
                "properties": {
                    "a": {"type": "integer", "description": "first integer addend"},
                    "b": {"type": "integer", "description": "second integer addend"}
                },
                "required": ["a", "b"]
            }
        }
    })"));
}

FString ULiteRtLmAddNumbersTool::Execute_Implementation(const FString& ArgumentsJson)
{
    // Parse arguments. Small models often emit numeric arguments as
    // JSON strings rather than numbers (e.g. "a": "27" instead of
    // "a": 27), so we accept both — see the Phase 1 ToolCallTest's
    // ExecuteAddNumbersTool helper for the same pattern.
    TSharedPtr<FJsonObject> ArgsObj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
    if (!FJsonSerializer::Deserialize(Reader, ArgsObj) || !ArgsObj.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("add_numbers: failed to parse arguments JSON: %s"),
               *ArgumentsJson);
        return FString(TEXT("\"ERROR: failed to parse arguments JSON\""));
    }

    auto ReadIntField = [&](const TCHAR* FieldName, int64& Out) -> bool
    {
        double AsNumber = 0.0;
        if (ArgsObj->TryGetNumberField(FieldName, AsNumber))
        {
            Out = static_cast<int64>(AsNumber);
            return true;
        }
        FString AsString;
        if (ArgsObj->TryGetStringField(FieldName, AsString))
        {
            if (AsString.IsNumeric())
            {
                Out = FCString::Atoi64(*AsString);
                return true;
            }
        }
        return false;
    };

    int64 A = 0;
    int64 B = 0;
    if (!ReadIntField(TEXT("a"), A))
    {
        return FString(TEXT("\"ERROR: missing or non-numeric argument 'a'\""));
    }
    if (!ReadIntField(TEXT("b"), B))
    {
        return FString(TEXT("\"ERROR: missing or non-numeric argument 'b'\""));
    }

    const int64 Sum = A + B;
    UE_LOG(LogInoAgents, Log,
           TEXT("add_numbers: %lld + %lld = %lld"), A, B, Sum);

    // Return as a bare JSON number. The conversation worker will
    // embed this verbatim into the tool_response.value field, which
    // LiteRT-LM's Gemma 4 data processor renders as "<tool>{value:42}"
    // in the text the model sees. See the Phase 1 ToolCallTest
    // comments for the exact rendering details.
    return FString::Printf(TEXT("%lld"), Sum);
}
