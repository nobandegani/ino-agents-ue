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
    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTtsNano: Voice: RegisterFromJsonFile (path=%s, register_as=%s)"),
           *JsonPath, *RegisterAs.ToString());

    if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*JsonPath))
    {
        OutError = FString::Printf(TEXT("Voice file not found: %s"), *JsonPath);
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNano: Voice: %s"), *OutError);
        return false;
    }

    FString Raw;
    if (!FFileHelper::LoadFileToString(Raw, *JsonPath))
    {
        OutError = FString::Printf(TEXT("Failed to read %s"), *JsonPath);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNano: Voice: %s"), *OutError);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = FString::Printf(TEXT("Failed to parse JSON in %s"), *JsonPath);
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNano: Voice: %s"), *OutError);
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

    const int32 NCodes = Voice.RefCodes.Num();
    const int32 RefTextLen = Voice.RefText.Len();
    const int32 RefPhonesLen = Voice.RefPhones.Len();

    Register(RegisterAs, MoveTemp(Voice));

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano: Voice: registered \"%s\" as name=%s "
                "(parsed %d ref codes, %d-char ref_text, %d-char ref_phones, registry_size=%d)"),
           *Voices[RegisterAs].DisplayName, *RegisterAs.ToString(),
           NCodes, RefTextLen, RefPhonesLen, Voices.Num());
    return true;
}

void FInoNeuTtsNanoVoiceRegistry::Register(FName Name, FInoNeuTtsNanoVoice Voice)
{
    const bool bReplacing = Voices.Contains(Name);
    Voices.Add(Name, MoveTemp(Voice));
    if (bReplacing)
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("NeuTtsNano: Voice: Register replaced existing entry for name=%s"),
               *Name.ToString());
    }
}

const FInoNeuTtsNanoVoice* FInoNeuTtsNanoVoiceRegistry::Find(FName Name) const
{
    const FInoNeuTtsNanoVoice* Result = Voices.Find(Name);
    UE_LOG(LogInoAgents, Verbose,
           TEXT("NeuTtsNano: Voice: Find(name=%s) — %s"),
           *Name.ToString(),
           Result ? TEXT("hit") : TEXT("miss"));
    return Result;
}

TArray<FName> FInoNeuTtsNanoVoiceRegistry::GetAvailableVoiceNames() const
{
    TArray<FName> Names;
    Voices.GetKeys(Names);
    return Names;
}
