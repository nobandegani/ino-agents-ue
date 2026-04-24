// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmAddNumbersTool.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"

UInoLiteRtLmAddNumbersTool::UInoLiteRtLmAddNumbersTool()
{
    ToolName = TEXT("add_numbers");
    Description = TEXT("Adds two integers and returns their sum. Always use this "
                       "tool when the user asks to add, sum, or total two numbers "
                       "— do not compute in your head.");

    FInoLiteRtLmToolParameter ParamA;
    ParamA.Name = TEXT("a");
    ParamA.Type = TEXT("integer");
    ParamA.Description = TEXT("first integer addend");
    ParamA.bRequired = true;
    Parameters.Add(ParamA);

    FInoLiteRtLmToolParameter ParamB;
    ParamB.Name = TEXT("b");
    ParamB.Type = TEXT("integer");
    ParamB.Description = TEXT("second integer addend");
    ParamB.bRequired = true;
    Parameters.Add(ParamB);
}

FString UInoLiteRtLmAddNumbersTool::Execute_Implementation(const FJsonObjectWrapper& Arguments)
{
    if (!Arguments.JsonObject.IsValid())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Tool: add_numbers — arguments object is null"));
        return FString(TEXT("\"ERROR: failed to parse arguments\""));
    }

    // Small models often emit numeric arguments as JSON strings rather
    // than numbers (e.g. "a": "27" instead of "a": 27), so we accept both.
    auto ReadIntField = [&](const TCHAR* FieldName, int64& Out) -> bool
    {
        double AsNumber = 0.0;
        if (Arguments.JsonObject->TryGetNumberField(FieldName, AsNumber))
        {
            Out = static_cast<int64>(AsNumber);
            return true;
        }
        FString AsString;
        if (Arguments.JsonObject->TryGetStringField(FieldName, AsString))
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
           TEXT("LiteRtLm: Tool: add_numbers(%lld, %lld) = %lld"), A, B, Sum);

    return FString::Printf(TEXT("%lld"), Sum);
}
