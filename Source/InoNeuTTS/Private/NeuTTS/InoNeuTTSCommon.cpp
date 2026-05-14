// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSCommon.h"

#include "InoNeuTTSSettings.h"
#include "NeuTTS/InoNeuTTSTypes.h"

namespace InoNeuTTSNative
{

FString ResolveBackbonePath(const FString& NameOrFileName)
{
    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings) return FString();
    const FInoNeuTTSBackboneEntry* Entry = Settings->FindBackbone(NameOrFileName);
    if (!Entry) return FString();
    return UInoNeuTTSSettings::ResolveLocalPath(Entry->LocalFileName);
}

FString ResolveDecoderPath(const FString& NameOrFileName)
{
    const UInoNeuTTSSettings* Settings = UInoNeuTTSSettings::Get();
    if (!Settings) return FString();
    const FInoNeuTTSDecoderEntry* Entry = Settings->FindDecoder(NameOrFileName);
    if (!Entry) return FString();
    return UInoNeuTTSSettings::ResolveLocalPath(Entry->LocalFileName);
}

FString NormalizePhones(const FString& Phones)
{
    FString Out;
    Out.Reserve(Phones.Len());
    bool bLastWasSpace = true;  // start true → leading whitespace is trimmed
    for (TCHAR C : Phones)
    {
        if (FChar::IsWhitespace(C))
        {
            if (!bLastWasSpace)
            {
                Out += TEXT(' ');
                bLastWasSpace = true;
            }
        }
        else
        {
            Out += C;
            bLastWasSpace = false;
        }
    }
    // Trim a trailing space if the loop's final char was whitespace.
    if (Out.Len() > 0 && Out[Out.Len() - 1] == TEXT(' '))
    {
        Out.RemoveAt(Out.Len() - 1);
    }
    return Out;
}

} // namespace InoNeuTTSNative
