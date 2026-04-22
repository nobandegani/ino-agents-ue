// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoVoiceRegistry.h"

#include "InoAgentsLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FInoNeuTtsNanoVoiceRegistry::RegisterFromJsonFile(
    const FString& JsonPath, FName RegisterAs, FString& OutError)
{
    if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*JsonPath))
    {
        OutError = FString::Printf(TEXT("Voice file not found: %s"), *JsonPath);
        return false;
    }

    FString Raw;
    if (!FFileHelper::LoadFileToString(Raw, *JsonPath))
    {
        OutError = FString::Printf(TEXT("Failed to read %s"), *JsonPath);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = FString::Printf(TEXT("Failed to parse JSON in %s"), *JsonPath);
        return false;
    }

    FInoNeuTtsNanoVoice Voice;
    Root->TryGetStringField(TEXT("display_name"), Voice.DisplayName);
    Root->TryGetStringField(TEXT("ref_text"),     Voice.RefText);
    Root->TryGetStringField(TEXT("ref_phones"),   Voice.RefPhones);

    const TArray<TSharedPtr<FJsonValue>>* CodesArray = nullptr;
    if (Root->TryGetArrayField(TEXT("ref_codes"), CodesArray) && CodesArray != nullptr)
    {
        Voice.RefCodes.Reserve(CodesArray->Num());
        for (const TSharedPtr<FJsonValue>& V : *CodesArray)
        {
            if (V.IsValid())
            {
                // JSON numbers come back as double; cast to int32. FSQ
                // codes are 16-bit (0-65535) so no precision loss at
                // int32.
                Voice.RefCodes.Add((int32)V->AsNumber());
            }
        }
    }

    // DisplayName defaults to the registration name if the JSON didn't
    // provide one — avoids an empty UI string.
    if (Voice.DisplayName.IsEmpty())
    {
        Voice.DisplayName = RegisterAs.ToString();
    }

    Register(RegisterAs, MoveTemp(Voice));
    return true;
}

void FInoNeuTtsNanoVoiceRegistry::Register(FName Name, FInoNeuTtsNanoVoice Voice)
{
    Voices.Add(Name, MoveTemp(Voice));
}

const FInoNeuTtsNanoVoice* FInoNeuTtsNanoVoiceRegistry::Find(FName Name) const
{
    return Voices.Find(Name);
}

TArray<FName> FInoNeuTtsNanoVoiceRegistry::GetAvailableVoiceNames() const
{
    TArray<FName> Names;
    Voices.GetKeys(Names);
    return Names;
}
